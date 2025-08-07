/**
 * @file adce.c
 * @brief 实现一个优化的激进死代码消除（Aggressive Dead Code Elimination, ADCE）遍。
 * @details
 * 本文件实现了 ADCE 算法，用于移除程序中无用的指令。其核心思想是
 * 一个基于工作列表（Worklist）的标记-清扫（Mark-and-Sweep）算法：
 * 1.  **标记关键指令**: 首先将所有具有副作用的指令（如 store, call, ret）
 *     识别为"关键指令"（critical），并将它们放入一个工作列表中。这些指令
 *     被认为是"活"的（live）。
 * 2.  **传播存活性**: 不断从工作列表中取出一条活指令，然后：
 *     a.  将其所有操作数的定义指令也标记为活的，并加入工作列表（数据流传播）。
 *     b.  将其所在基本块的所有前驱块的终结符指令也标记为活的（控制流传播）。
 * 3.  **清扫**: 遍历所有指令，将所有未被标记为活的指令移除。
 * 这种方法可以有效地消除那些其结果仅被其他死代码使用的指令链。
 */
#include "ir/transforms/adce.h"
#include "ir/ir_utils.h"
#include <string.h>
#include "logger.h"       // for LOG_CATEGORY_IR_OPT, LOG_DEBUG, LOG_WARN

// --- 用于 ADCE 分析的内部数据结构 ---

/**
 * @struct InstructionInfo
 * @brief 存储单条指令的预计算信息，以避免在主循环中重复计算。
 */
typedef struct {
    IRInstruction* instr;   ///< 指向指令本身
    bool is_critical;       ///< 是否为关键指令
    bool is_terminator;     ///< 是否为终结符指令
    bool is_phi;            ///< 是否为 PHI 节点
} InstructionInfo;

/**
 * @struct BlockInfo
 * @brief 存储单个基本块的分析信息。
 */
typedef struct {
    IRBasicBlock* bb;           ///< 指向基本块本身
    bool is_live;               ///< 此块是否被标记为活的（即包含活指令）
    int live_instruction_count; ///< 块内活指令的数量
    int total_instruction_count;///< 块内总指令数
} BlockInfo;

// --- 辅助函数原型声明 ---
static bool is_critical_instruction(IRInstruction* instr);
static void mark_instruction_live(IRInstruction* instr, Worklist* wl, bool* live_blocks, BlockInfo* block_info);
static void propagate_data_flow_liveness(IRInstruction* instr, Worklist* wl, bool* live_blocks, BlockInfo* block_info);
static void propagate_control_flow_liveness(IRBasicBlock* bb, Worklist* wl, bool* live_blocks, BlockInfo* block_info);
static void initialize_instruction_info(IRFunction* func, InstructionInfo* instr_info, BlockInfo* block_info);

// --- 主要的 ADCE 优化遍入口 ---
bool run_adce(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "ADCE: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running ADCE on function @%s", func->name);
    }
    
    // --- 1. 初始化数据结构 ---
    MemoryPool* pool = func->module->pool;
    bool changed = false;
    
    // 使用缓存的指令计数，避免重复遍历
    int total_instructions = func->instruction_count;
    if (total_instructions == 0) {
        // 如果缓存值为0，重新计算以确保准确性
        recalculate_instruction_count(func);
        total_instructions = func->instruction_count;
    }
    
    Worklist* wl = create_worklist(pool, total_instructions);
    bool* live_blocks = (bool*)pool_alloc_z(pool, func->block_count * sizeof(bool));
    InstructionInfo* instr_info = (InstructionInfo*)pool_alloc_z(pool, total_instructions * sizeof(InstructionInfo));
    BlockInfo* block_info = (BlockInfo*)pool_alloc_z(pool, func->block_count * sizeof(BlockInfo));
    
    // --- 2. 预计算指令和块的信息 ---
    initialize_instruction_info(func, instr_info, block_info);
    
    // --- 3. 初始时将所有指令标记为死 ---
    for (IRBasicBlock* bb = func->blocks; bb; bb = bb->next_in_func) {
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            instr->is_live = false;
        }
    }
    
    // --- 4. 用所有关键指令初始化工作列表 ---
    int instr_idx = 0;
    for (IRBasicBlock* bb = func->blocks; bb; bb = bb->next_in_func) {
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            // 使用指令索引来访问预计算的指令信息
            InstructionInfo* ii = &instr_info[instr_idx];
            if (ii->is_critical) {
                mark_instruction_live(instr, wl, live_blocks, block_info);
            }
            instr_idx++;
        }
    }
    
    // --- 5. 使用工作列表算法传播存活性 ---
    int iteration_count = 0;
    const int max_iterations = total_instructions * 2; // 安全上限，防止无限循环

    while (wl->count > 0 && iteration_count < max_iterations) {
        iteration_count++;
        IRInstruction* live_instr = (IRInstruction*)worklist_pop(wl);
        
        // 传播数据流存活性：一条指令是活的，那么它的操作数的定义指令也是活的。
        propagate_data_flow_liveness(live_instr, wl, live_blocks, block_info);
        
        // 传播控制流存活性：一条指令是活的，那么其所在基本块的控制流依赖（即前驱的终结符）也是活的。
        if (live_instr->parent) {
            propagate_control_flow_liveness(live_instr->parent, wl, live_blocks, block_info);
        }
    }
    
    if (iteration_count >= max_iterations) {
        if (func && func->module && func->module->log_config) {
            LOG_WARN(func->module->log_config, LOG_CATEGORY_IR_OPT, "ADCE: Reached maximum iterations in function @%s", func->name);
        }
    }
    
    // --- 6. 清扫阶段：移除所有未被标记为活的指令 ---
    int removed_count = 0;
    for (IRBasicBlock* bb = func->blocks; bb; bb = bb->next_in_func) {
        IRInstruction* instr = bb->head;
        while (instr) {
            IRInstruction* next_instr = instr->next;
            if (!instr->is_live) {
                erase_instruction(instr);
                removed_count++;
                changed = true;
            }
            instr = next_instr;
        }
    }
    
    if (changed) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "ADCE: Removed %d dead instructions in function @%s", removed_count, func->name);
        }
    }
    
    return changed;
}

// --- 增强的辅助函数 ---

/**
 * @brief 判断一条指令是否为"关键指令"（即本身具有不可消除的副作用）。
 */
static bool is_critical_instruction(IRInstruction* instr) {
    if (!instr) return false;
    
    switch (instr->opcode) {
        case IR_OP_CALL:            // 函数调用可能有未知副作用
        case IR_OP_STORE:           // 内存写入是副作用
        case IR_OP_RET:             // 函数返回是关键的控制流
        case IR_OP_BR:              // 分支是关键的控制流
            return true;
        default:
            return false;
    }
}

/**
 * @brief 预计算并初始化指令和块的信息。
 */
static void initialize_instruction_info(IRFunction* func, InstructionInfo* instr_info, BlockInfo* block_info) {
    int instr_idx = 0;
    
    for (IRBasicBlock* bb = func->blocks; bb; bb = bb->next_in_func) {
        BlockInfo* bi = &block_info[bb->post_order_id];
        bi->bb = bb;
        bi->is_live = false;
        bi->live_instruction_count = 0;
        bi->total_instruction_count = 0;
        
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            InstructionInfo* ii = &instr_info[instr_idx];
            ii->instr = instr;
            ii->is_critical = is_critical_instruction(instr);
            ii->is_terminator = (instr->opcode == IR_OP_RET || instr->opcode == IR_OP_BR);
            ii->is_phi = (instr->opcode == IR_OP_PHI);
            
            bi->total_instruction_count++;
            instr_idx++;
        }
    }
}

/**
 * @brief 将一条指令标记为活的，并将其加入工作列表。
 */
static void mark_instruction_live(IRInstruction* instr, Worklist* wl, bool* live_blocks, BlockInfo* block_info) {
    if (!instr || instr->is_live) return;
    
    instr->is_live = true;
    worklist_add(wl, instr);
    
    // 同时标记其所在的块为活的。
    if (instr->parent) {
        int block_id = instr->parent->post_order_id;
        if (!live_blocks[block_id]) {
            live_blocks[block_id] = true;
            block_info[block_id].is_live = true;
        }
        block_info[block_id].live_instruction_count++;
    }
}

/**
 * @brief 沿着数据流反向传播存活性。
 * @details 如果指令 I 是活的，那么定义其操作数的所有指令也必须是活的。
 */
static void propagate_data_flow_liveness(IRInstruction* instr, Worklist* wl, bool* live_blocks, BlockInfo* block_info) {
    if (!instr) return;
    
    if (instr->opcode == IR_OP_PHI) {
        // 更激进的 PHI 处理：只考虑活前驱块的入口
        for (IROperand* op = instr->operand_head; op; ) {
            IRValue* val = op->data.value;
            IROperand* op_block = op->next_in_instr;
            if (op_block && op_block->kind == IR_OP_KIND_BASIC_BLOCK) {
                IRBasicBlock* pred_bb = op_block->data.bb;
                if (live_blocks[pred_bb->post_order_id]) {
                    if (val && !val->is_constant && val->def_instr) {
                        mark_instruction_live(val->def_instr, wl, live_blocks, block_info);
                    }
                }
            }
            op = op_block ? op_block->next_in_instr : NULL;
        }
    } else {
        // Regular instruction: mark all operand definitions as live
        for (IROperand* op = instr->operand_head; op; op = op->next_in_instr) {
            IRValue* val = op->data.value;
            if (val && !val->is_constant && val->def_instr) {
                mark_instruction_live(val->def_instr, wl, live_blocks, block_info);
            }
        }
    }
}

/**
 * @brief 沿着控制流反向传播存活性。
 * @details 如果一个块 B 是活的，那么所有能够跳转到 B 的终结符指令也必须是活的。
 */
static void propagate_control_flow_liveness(IRBasicBlock* bb, Worklist* wl, bool* live_blocks, BlockInfo* block_info) {
    if (!bb) return;
    
    int block_id = bb->post_order_id;
    if (!live_blocks[block_id]) {
        live_blocks[block_id] = true;
        block_info[block_id].is_live = true;
        
        // 将所有前驱块的终结符指令标记为活的。
        for (int i = 0; i < bb->num_predecessors; ++i) {
            IRBasicBlock* pred_bb = bb->predecessors[i];
            if (pred_bb && pred_bb->tail) {
                mark_instruction_live(pred_bb->tail, wl, live_blocks, block_info);
            }
        }
    }
}