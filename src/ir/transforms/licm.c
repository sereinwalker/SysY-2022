/**
 * @file licm.c
 * @brief 实现循环不变量外提（Loop-Invariant Code Motion, LICM）优化遍。
 * @details
 * 本文件实现了 LICM 算法，用于将循环内部的、每次迭代结果都相同的计算
 * 移动到循环外部。其核心流程如下：
 * 1.  **分析**: 首先运行循环分析，识别出函数中所有的自然循环及其嵌套结构。
 * 2.  **前置头确保**: 为每个循环创建一个"前置头"（preheader）。这是一个在进入
 *     循环之前只执行一次的基本块，为外提代码提供了一个安全的放置位置。
 *     如果创建了前置头，则需要重新运行 CFG 和支配树等分析。
 * 3.  **递归处理**: 从最内层的循环开始，递归地向外层循环处理。
 * 4.  **识别与外提**: 对每个循环：
 *     a.  **识别不变量**: 遍历循环内的所有指令，找出其操作数都在循环外部定义
 *         （或为常量）的指令。
 *     b.  **检查安全性**: 确认该指令可以被安全地外提（例如，不会引发异常，
 *         或其所在块支配所有循环出口）。
 *     c.  **执行外提**: 将满足条件的指令移动到循环的前置头中。
 * 5.  **迭代**: 重复步骤 4c，直到没有更多指令可以被外提。
 */
#include "ir/transforms/licm.h"
#include "ir/analysis/loop_analysis.h"
#include "ir/analysis/cfg_builder.h"
#include "ir/analysis/dominators.h"
#include "ir/ir_utils.h"
#include "ir/ir_builder.h"
#include <stdio.h>
#include "ast.h"                        // for pool_alloc
#include "logger.h"                     // for LOG_CATEGORY_IR_OPT, LOG_DEBUG


// --- 其他模块辅助函数的前向声明 ---
void ir_builder_set_insertion_block_end(IRBuilder* builder, IRBasicBlock* block);

// --- 用于 LICM 分析的内部数据结构 ---

/**
 * @struct HoistCandidate
 * @brief 存储一个可能被外提的指令及其相关分析信息。
 */
typedef struct {
    IRInstruction* instr;       ///< 指向候选指令
    IRBasicBlock* block;        ///< 指令所在的原始基本块
    bool is_safe_to_hoist;      ///< 标记是否可以安全地外提
    int hoist_priority;         ///< 外提的优先级（例如，优先移动计算密集的指令）
} HoistCandidate;

/**
 * @struct LICMContext
 * @brief 在处理单个循环时维护所有状态的上下文。
 */
typedef struct {
    Loop* loop;                 ///< 正在处理的循环
    HoistCandidate* candidates; ///< 候选指令数组
    int candidate_count;        ///< 候选指令数量
    int max_candidates;         ///< 候选指令数组容量
    bool changed;               ///< 标记本次处理是否对循环作出了修改
} LICMContext;

// --- 辅助函数原型声明 ---
static bool ensure_all_loop_preheaders(IRFunction* func);
static bool ensure_loop_preheader(Loop* loop, IRBuilder* builder);
static bool process_loop_recursively(Loop* loop);
static bool process_loop_for_licm(Loop* loop);
static bool is_loop_invariant(IRInstruction* instr, BitSet* loop_blocks_bs);
static bool can_hoist_instruction(IRInstruction* instr, Loop* loop);
static void hoist_instruction(IRInstruction* instr, IRBasicBlock* preheader);
static bool is_instruction_safe_to_speculate(IRInstruction* instr);
static void collect_hoist_candidates(LICMContext* ctx);
static int calculate_hoist_priority(IRInstruction* instr);
static bool dominates_all_exits(IRInstruction* instr, Loop* loop);
static void sort_hoist_candidates(HoistCandidate* candidates, int count);

// --- 主要入口点 ---
bool run_licm(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running LICM on function @%s", func->name);
    }
    
    bool changed_overall = false;

    // 1. 首先运行循环分析
    find_loops(func);
    if (!func->top_level_loops) {
        if (func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: No loops found in function @%s", func->name);
        }
        return false;
    }

    // 2. 确保所有循环都有前置头
    if (ensure_all_loop_preheaders(func)) {
        // 如果创建了前置头，CFG 就被修改了，需要重建所有依赖它的分析
        build_cfg(func);
        compute_dominators(func);
        find_loops(func); // 循环信息也需要重建
        changed_overall = true;
        if (func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: Rebuilt CFG after preheader creation");
        }
    }
    
    // 3. 递归地处理所有循环
    for (Loop* loop = func->top_level_loops; loop; loop = loop->next) {
        changed_overall |= process_loop_recursively(loop);
    }

    if (changed_overall && func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: Applied transformations in function @%s", func->name);
    }
    
    return changed_overall;
}

// --- 递归处理和前置头逻辑 ---

/** @brief 递归地处理一个循环及其所有子循环。*/
static bool process_loop_recursively(Loop* loop) {
    if (!loop) return false;
    
    bool changed = false;
    
    // 首先处理所有子循环（从最内层到最外层）
    for (int i = 0; i < loop->num_sub_loops; ++i) {
        changed |= process_loop_recursively(loop->sub_loops[i]);
    }
    
    // 然后处理当前循环
    changed |= process_loop_for_licm(loop);
    
    return changed;
}

/** @brief 确保一个函数内的所有循环都有前置头。*/
static bool ensure_all_loop_preheaders(IRFunction* func) {
    bool changed = false;
    
    // 使用工具函数获取按深度排序的循环列表
    Worklist* sorted_loops = get_loops_sorted_by_depth(func);
    if (!sorted_loops) return false;
    
    IRBuilder builder;
    ir_builder_init(&builder, func);

    // 为所有循环创建前置头
    for (int i = 0; i < sorted_loops->count; ++i) {
        Loop* loop = (Loop*)sorted_loops->items[i];
        if (ensure_loop_preheader(loop, &builder)) {
            changed = true;
        }
    }
    
    return changed;
}

/** @brief 为单个循环确保或创建一个前置头。*/
static bool ensure_loop_preheader(Loop* loop, IRBuilder* builder) {
    if (!loop || !loop->header) return false;
    
    IRBasicBlock* header = loop->header;
    
    // 找到所有来自循环外部的前驱块
    Worklist* outside_preds_wl = create_worklist(builder->module->pool, header->num_predecessors);
    for (int i = 0; i < header->num_predecessors; ++i) {
        IRBasicBlock* pred = header->predecessors[i];
        if (pred && !bitset_contains(loop->loop_blocks_bs, pred->post_order_id)) {
            worklist_add(outside_preds_wl, pred);
        }
    }
    
    int num_outside_preds = outside_preds_wl->count;
    if (num_outside_preds == 0) return false; // 没有外部前驱，可能是死循环

    // 检查是否已经存在一个合适的前置头
    IRBasicBlock* single_outside_pred = (num_outside_preds == 1) ? 
        (IRBasicBlock*)outside_preds_wl->items[0] : NULL;
    if (single_outside_pred && single_outside_pred->num_successors == 1) {
        loop->preheader = single_outside_pred;
        return false; // 已有前置头，无需修改
    }
    
    // 创建新的前置头基本块
    IRBasicBlock* preheader = ir_builder_create_block(builder, "loop.preheader");
    loop->preheader = preheader;
    
    // 注意：这里没有ASTContext，所以我们需要创建一个默认的LogConfig
    LogConfig default_log_config;
    logger_config_init_default(&default_log_config);
    
    LOG_DEBUG(&default_log_config, LOG_CATEGORY_IR_OPT, "LICM: Creating preheader %s for loop with header %s", 
              preheader->label, header->label);

    // 更新循环头部的 PHI 节点，将来自外部的入口合并到前置头
    for (IRInstruction* instr = header->head; instr && instr->opcode == IR_OP_PHI; instr = instr->next) {
        ir_builder_set_insertion_block_end(builder, preheader);
        IRInstruction* preheader_phi = ir_builder_create_phi(builder, instr->dest->type, "licm.phi");

        // 从原始 PHI 中移除外部入口，并添加到新的 preheader_phi 中
        for (int i = 0; i < num_outside_preds; ++i) {
            IRBasicBlock* pred = (IRBasicBlock*)outside_preds_wl->items[i];
            for (IROperand* op = instr->operand_head; op; ) {
                IROperand* block_op = op->next_in_instr;
                IROperand* next_val_op = block_op->next_in_instr;
                if ((IRBasicBlock*)block_op->data.bb == pred) {
                    ir_phi_add_incoming(preheader_phi, op->data.value, pred);
                    remove_operand(block_op);
                    remove_operand(op);
                    break;
                }
                op = next_val_op;
            }
        }
        // 在原始 PHI 中添加来自前置头的新入口
        ir_phi_add_incoming(instr, preheader_phi->dest, preheader);
    }
    
    // 将所有外部前驱的跳转目标重定向到新的前置头
    for (int i = 0; i < num_outside_preds; ++i) {
        IRBasicBlock* pred = (IRBasicBlock*)outside_preds_wl->items[i];
        redirect_edge(pred, header, preheader);
    }

    // 在前置头的末尾添加一个到循环头的无条件跳转
    ir_builder_set_insertion_block_end(builder, preheader);
    ir_builder_create_br(builder, header);
    
    return true;
}

// --- LICM 核心逻辑 ---

/** @brief 对单个循环执行 LICM 优化。*/
static bool process_loop_for_licm(Loop* loop) {
    if (!loop || !loop->preheader) {
        if (loop && loop->header && loop->header->parent && loop->header->parent->module && loop->header->parent->module->log_config) {
            LOG_WARN(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: Skipping loop with header %s (no preheader)", 
                     loop->header ? loop->header->label : "unknown");
        }
        return false;
    }

    LICMContext ctx = {0};
    ctx.loop = loop;
    ctx.max_candidates = 256;
    ctx.candidates = (HoistCandidate*)pool_alloc(loop->header->parent->module->pool, 
                                               ctx.max_candidates * sizeof(HoistCandidate));
    ctx.changed = false;

    // 迭代地外提指令，直到没有更多指令可以被外提
    while (true) {
        ctx.candidate_count = 0;
        collect_hoist_candidates(&ctx);
        
        if (ctx.candidate_count == 0) break;
        
        // 按优先级排序候选者
        sort_hoist_candidates(ctx.candidates, ctx.candidate_count);
        
        if (loop->header->parent->module && loop->header->parent->module->log_config) {
            LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: Hoisting %d instructions from loop with header %s", 
                      ctx.candidate_count, loop->header->label);
        }
        
        // 外提所有安全的候选指令
        for (int i = 0; i < ctx.candidate_count; ++i) {
            if (ctx.candidates[i].is_safe_to_hoist) {
                hoist_instruction(ctx.candidates[i].instr, loop->preheader);
            }
        }
        
        ctx.changed = true;
    }
    
    return ctx.changed;
}

/** @brief 收集一个循环中所有可以被外提的候选指令。*/
static void collect_hoist_candidates(LICMContext* ctx) {
    Loop* loop = ctx->loop;
    
    for (int i = 0; i < loop->num_blocks; ++i) {
        IRBasicBlock* bb = loop->blocks[i];
        if (!bb) continue;
        
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            if (ctx->candidate_count >= ctx->max_candidates) break;
            
            // 检查指令是否是循环不变量，并且满足外提条件
            if (is_loop_invariant(instr, loop->loop_blocks_bs) && 
                can_hoist_instruction(instr, loop)) {
                
                HoistCandidate* candidate = &ctx->candidates[ctx->candidate_count++];
                candidate->instr = instr;
                candidate->block = bb;
                // 安全性检查：指令是否可被推测执行，且其定义块支配所有循环出口
                candidate->is_safe_to_hoist = is_instruction_safe_to_speculate(instr) && 
                                            dominates_all_exits(instr, loop);
                candidate->hoist_priority = calculate_hoist_priority(instr);
            }
        }
    }
}

/** @brief 计算一条指令的外提优先级。*/
static int calculate_hoist_priority(IRInstruction* instr) {
    if (!instr) return 0;
    
    // 更值得外提的指令有更高的优先级
    switch (instr->opcode) {
        case IR_OP_ADD:
        case IR_OP_SUB:
        case IR_OP_MUL:
        case IR_OP_SDIV:
        case IR_OP_SREM:
            return 10; // 算术运算
        case IR_OP_LOAD:
            return 5;  // 内存加载
        case IR_OP_CALL:
            return 3;  // 函数调用
        default:
            return 1;  // 其他操作
    }
}

/** @brief 对候选指令按优先级进行排序。*/
static void sort_hoist_candidates(HoistCandidate* candidates, int count) {
    // 简单的冒泡排序（高优先级在前）
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (candidates[j].hoist_priority < candidates[j+1].hoist_priority) {
                HoistCandidate temp = candidates[j];
                candidates[j] = candidates[j+1];
                candidates[j+1] = temp;
            }
        }
    }
}

/** @brief 检查一条指令的定义块是否支配所有循环出口。*/
static bool dominates_all_exits(IRInstruction* instr, Loop* loop) {
    if (!instr || !loop || !instr->parent) return false;
    
    IRBasicBlock* instr_block = instr->parent;
    for (int i = 0; i < loop->num_exit_blocks; ++i) {
        if (!dominates(instr_block, loop->exit_blocks[i])) {
            return false;
        }
    }
    return true;
}

// --- LICM 辅助函数实现 ---

/** @brief 检查一条指令是否是循环不变量。*/
static bool is_loop_invariant(IRInstruction* instr, BitSet* loop_blocks_bs) {
    if (!is_instruction_safe_to_speculate(instr)) return false;

    // 检查所有操作数
    for (IROperand* op = instr->operand_head; op; op = op->next_in_instr) {
        IRValue* val = op->data.value;
        if (val->is_constant) continue; // 常量总是不变的
        IRInstruction* def_instr = val->def_instr;
        // 如果操作数的定义在循环内部，那么此指令就不是不变量
        if (def_instr && bitset_contains(loop_blocks_bs, def_instr->parent->post_order_id)) {
            return false;
        }
    }
    return true;
}

/** @brief 检查一条指令是否可以被外提。*/
static bool can_hoist_instruction(IRInstruction* instr, Loop* loop) {
    IRBasicBlock* instr_block = instr->parent;
    // 必须支配所有循环出口
    for (int i = 0; i < loop->num_exit_blocks; ++i) {
        if (!dominates(instr_block, loop->exit_blocks[i])) {
            if (loop->header->parent->module && loop->header->parent->module->log_config) {
                LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: Cannot hoist instruction in block %s because it does not dominate exit block %s", instr_block->label, loop->exit_blocks[i]->label);
            }
            return false;
        }
    }
    // 对于可能抛出异常的指令，还必须支配所有 latch block（即 back_edges）
    if (!is_instruction_safe_to_speculate(instr)) {
        for (int i = 0; i < loop->num_back_edges; ++i) {
            if (!dominates(instr_block, loop->back_edges[i])) {
                if (loop->header->parent->module && loop->header->parent->module->log_config) {
                    LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LICM: Cannot hoist potentially trapping instruction in block %s because it does not dominate latch block %s", instr_block->label, loop->back_edges[i]->label);
                }
                return false;
            }
        }
    }
    return true;
}

/** @brief 将一条指令移动到循环的前置头。*/
static void hoist_instruction(IRInstruction* instr, IRBasicBlock* preheader) {
    // 1. 从原始块的链表中解开
    if (instr->prev) instr->prev->next = instr->next;
    else instr->parent->head = instr->next;
    if (instr->next) instr->next->prev = instr->prev;
    else instr->parent->tail = instr->prev;

    // 2. 插入到前置头的终结符之前
    insert_instr_before(instr, preheader->tail);
}

/** @brief 检查一条指令是否可以被安全地推测执行。*/
static bool is_instruction_safe_to_speculate(IRInstruction* instr) {
    switch (instr->opcode) {
        // 这些指令不会产生异常或副作用，可以安全地提前执行
        case IR_OP_ADD: case IR_OP_SUB: case IR_OP_MUL:
        case IR_OP_FADD: case IR_OP_FSUB: case IR_OP_FMUL:
        case IR_OP_SHL: case IR_OP_ASHR: case IR_OP_LSHR:
        case IR_OP_AND: case IR_OP_OR: case IR_OP_XOR:
        case IR_OP_ICMP: case IR_OP_FCMP:
        case IR_OP_GETELEMENTPTR: case IR_OP_PHI:
            return instr->dest != NULL;
        // 除法/取余可能因除零而异常
        case IR_OP_SDIV: case IR_OP_SREM: case IR_OP_FDIV:
        // load 可能因空指针或非法地址而异常
        case IR_OP_LOAD:
        // 这些指令明确有副作用
        case IR_OP_STORE: case IR_OP_CALL:
            return false;
        default:
            return false;
    }
}