/**
 * @file loop_unroll.c
 * @brief 实现一个针对 SysY 语言 `while` 循环的循环展开优化遍。
 * @details
 * 本文件实现了一个保守但健壮的循环展开（Loop Unroll）优化。
 * 它采用标准的"部分展开"算法，专注于处理由 SysY 的 `while` 循环生成的、
 * 具有典型归纳变量模式的简单循环。
 *
 * 为了保证转换的正确性和简化实现，此优化遍遵循以下策略：
 * 1.  **精确分析**：只处理具有简单控制流（单前继、单回边、单出口）的循环。
 * 2.  **编译期迭代次数**：只对编译时能精确计算出总迭代次数的循环进行操作。
 * 3.  **整除约束**：只在总迭代次数能被展开因子整除时才进行展开，以避免生成
 *     处理剩余迭代的"收尾循环"的复杂逻辑。
 * 4.  **SSA 维护**：在展开过程中，通过精确的值重映射（remapping）来保证
 *     SSA（静态单赋值）形式的正确性。
 */
#include "ir/transforms/loop_unroll.h"
#include "ir/analysis/loop_analysis.h"
#include "ir/analysis/dominators.h"
#include "ir/transforms/simplify_cfg.h"
#include "ir/ir_utils.h"
#include "ir/ir_builder.h"
#include "ir/ir_data_structures.h"
#include <string.h>
#include "logger.h"                      // for LOG_CATEGORY_IR_OPT, LOG_DEBUG


// --- 配置与启发式规则 ---
#define DEFAULT_UNROLL_FACTOR 4       // 默认展开因子
#define MAX_UNROLL_THRESHOLD 256      // 启发式规则：如果循环体指令数超过此阈值，则不进行展开

// --- 数据结构 ---

/**
 * @brief 存储规范化归纳变量（Canonical Induction Variable）的分析信息。
 */
typedef struct {
    bool is_canonical;           ///< 标记是否找到了一个规范化的归纳变量
    IRInstruction* phi;          ///< 代表归纳变量的 PHI 指令
    IRInstruction* cmp;          ///< 循环的退出条件比较指令
    IRInstruction* update_instr; ///< 归纳变量的更新指令（例如 i = i + 1）
    IRValue* start_val;          ///< 归纳变量的初始值
    IRValue* limit_val;          ///< 循环的边界值
    IRValue* step_val;           ///< 归纳变量的步进值（必须是常量）
    int trip_count;              ///< 编译时计算出的循环迭代总次数
} CanonicalIVInfo;

// --- 外部辅助函数的前向声明 ---
Worklist* get_loops_sorted_by_depth(IRFunction* func);
void replace_all_uses_with(Worklist* wl, IRValue* old_val, IRValue* new_val);

// --- 本文件内静态函数的原型声明 ---
static bool unroll_loop(Loop* loop, int unroll_factor);
static bool analyze_loop_for_unrolling(Loop* loop, CanonicalIVInfo* iv_info);
static void perform_unroll(Loop* loop, int unroll_factor, const CanonicalIVInfo* iv_info);
static void find_canonical_iv(Loop* loop, CanonicalIVInfo* iv_info);

// --- 主入口函数 ---

/**
 * @brief 对一个函数内的所有循环执行循环展开优化。
 * @param func 要优化的函数。
 * @return 如果对函数进行了任何修改，则返回 true。
 */
bool run_loop_unroll(IRFunction* func) {
    if (!func || !func->entry) return false;
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running LoopUnroll on function @%s", func->name);
    }
    
    if (!func->top_level_loops) {
        if (func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "LoopUnroll: No loops found in @%s", func->name);
        }
        return false;
    }

    bool changed_overall = false;
    // 从内层向外层处理循环，这样内层循环展开后可能为外层循环创造更多优化机会
    Worklist* sorted_loops = get_loops_sorted_by_depth(func);

    for (int i = 0; i < sorted_loops->count; ++i) {
        Loop* loop = (Loop*)sorted_loops->items[i];
        if (unroll_loop(loop, DEFAULT_UNROLL_FACTOR)) {
            changed_overall = true;
        }
    }

    if (changed_overall) {
        // 循环展开会改变CFG，通常需要运行CFG简化来清理产生的冗余块
        run_simplify_cfg(func);
        // CFG改变后，支配关系也需要重新计算
        compute_dominators(func);
    }
    return changed_overall;
}

// --- 每个循环的处理逻辑 ---

/**
 * @brief 尝试对单个循环进行展开。
 * @param loop 要展开的循环。
 * @param unroll_factor 展开因子。
 * @return 如果成功展开，返回 true。
 */
static bool unroll_loop(Loop* loop, int unroll_factor) {
    CanonicalIVInfo iv_info;

    // 1. 分析循环是否适合展开
    if (!analyze_loop_for_unrolling(loop, &iv_info)) {
        return false;
    }

    // 2. 检查迭代次数是否能被整除（保守策略，避免收尾循环的复杂性）
    if (iv_info.trip_count % unroll_factor != 0) {
        if (loop->header->parent->module && loop->header->parent->module->log_config) {
            LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LoopUnroll: Skipping loop %s, trip count %d is not divisible by factor %d.",
                       loop->header->label, iv_info.trip_count, unroll_factor);
        }
        return false;
    }

    if (loop->header->parent->module && loop->header->parent->module->log_config) {
        LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LoopUnroll: Unrolling loop with header %s (trip count: %d, factor: %d)",
                  loop->header->label, iv_info.trip_count, unroll_factor);
    }

    // 3. 执行展开转换
    perform_unroll(loop, unroll_factor, &iv_info);
    
    return true;
}

/**
 * @brief 分析循环是否满足展开的先决条件。
 * @param loop 要分析的循环。
 * @param iv_info 用于存储归纳变量分析结果的结构体。
 * @return 如果适合展开，返回 true。
 */
static bool analyze_loop_for_unrolling(Loop* loop, CanonicalIVInfo* iv_info) {
    // A. 必须是简单的循环结构：一个前继块，一个回边，一个退出块
    if (!loop->preheader || loop->num_exit_blocks != 1 || loop->num_back_edges != 1) {
        if (loop->header->parent->module && loop->header->parent->module->log_config) {
            LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LoopUnroll: Skipping loop %s due to complex CFG.", loop->header->label);
        }
        return false;
    }

    // B. 启发式规则：不展开过于庞大的循环，以控制代码膨胀
    int instr_count = 0;
    for (int i = 0; i < loop->num_blocks; ++i) {
        IRInstruction* instr = loop->blocks[i]->head;
        while (instr) {
            instr_count++;
            instr = instr->next;
        }
    }
    if (instr_count > MAX_UNROLL_THRESHOLD) {
        if (loop->header->parent->module && loop->header->parent->module->log_config) {
            LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LoopUnroll: Skipping loop %s due to size heuristics (instr_count: %d)", loop->header->label, instr_count);
        }
        return false;
    }

    // C. 必须能找到一个规范化的归纳变量，并且迭代次数在编译期可知
    find_canonical_iv(loop, iv_info);
    if (!iv_info->is_canonical || iv_info->trip_count < 0) {
        if (loop->header->parent->module && loop->header->parent->module->log_config) {
            LOG_DEBUG(loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "LoopUnroll: Skipping loop %s, not in canonical form or trip count is unknown.", loop->header->label);
        }
        return false;
    }
    
    return true;
}

/**
 * @brief 执行实际的循环展开变换。
 * @param loop 要展开的循环。
 * @param unroll_factor 展开因子。
 * @param iv_info 归纳变量信息。
 */
static void perform_unroll(Loop* loop, int unroll_factor, const CanonicalIVInfo* iv_info) {
    IRBasicBlock* preheader = loop->preheader;
    IRBasicBlock* header = loop->header;
    IRBasicBlock* latch = loop->back_edges[0]; // 简单循环只有一个回边块
    IRFunction* func = header->parent;
    MemoryPool* pool = func->module->pool;

    // --- 1. 克隆循环体 `unroll_factor - 1` 次 ---
    ValueMap value_remap; // 映射原始 SSA 值到其在最近一次展开副本中的对应值
    value_map_init(&value_remap, pool);

    // 初始映射：将 PHI 节点的结果映射到其来自 preheader 的初始值
    for (IRInstruction* phi = header->head; phi && phi->opcode == IR_OP_PHI; phi = phi->next) {
        for (IROperand* op = phi->operand_head; op; op = op->next_in_instr->next_in_instr) {
            if (op->next_in_instr->data.bb == preheader) {
                value_map_put(&value_remap, phi->dest, op->data.value, func->module->log_config);
                break;
            }
        }
    }
    
    IRBasicBlock* last_block_in_chain = latch;

    for (int i = 1; i < unroll_factor; ++i) {
        ValueMap iter_remap; // 当前这次克隆的局部映射： original_value -> cloned_value
        value_map_init(&iter_remap, pool);
        
        // 克隆循环中的所有基本块
        for (int j = 0; j < loop->num_blocks; j++) {
            IRBasicBlock* old_bb = loop->blocks[j];
            IRBasicBlock* new_bb = ir_builder_create_block(NULL, old_bb->label);
            link_basic_block_to_function(new_bb, func);
            value_map_put(&iter_remap, (IRValue*)old_bb, (IRValue*)new_bb, func->module->log_config);
        }

        // 克隆并重映射所有指令
        for (int j = 0; j < loop->num_blocks; j++) {
            IRBasicBlock* old_bb = loop->blocks[j];
            IRBasicBlock* new_bb = (IRBasicBlock*)value_map_get(&iter_remap, (IRValue*)old_bb, func->module->log_config);
            
            for (IRInstruction* old_instr = old_bb->head; old_instr; old_instr = old_instr->next) {
                // PHI 节点在循环体副本中没有意义，直接跳过
                if (old_instr->opcode == IR_OP_PHI) continue;

                IRInstruction* new_instr = clone_instruction(old_instr, pool);
                
                // 为新指令创建目标寄存器（如果有的话）
                if (old_instr->dest) {
                    // 需要为新指令创建一个新的目标寄存器
                    IRBuilder temp_builder;
                    ir_builder_init(&temp_builder, func);
                    new_instr->dest = ir_builder_create_reg(&temp_builder, old_instr->dest->type, old_instr->dest->name);
                    new_instr->dest->def_instr = new_instr;
                    // 记录 original -> clone 的映射
                    value_map_put(&iter_remap, old_instr->dest, new_instr->dest, func->module->log_config);
                }
                
                // 使用 `value_remap` 来重映射操作数，使其引用前一个迭代副本产生的值
                remap_instruction_operands(new_instr, &value_remap);
                // 对于块内引用，需要用 `iter_remap` 再次重映射
                remap_instruction_operands(new_instr, &iter_remap);

                add_instr_to_bb_end(new_bb, new_instr);
            }
        }

        // 将上一个块（可能是原始的latch或上一个克隆的latch）的终结符重定向到当前克隆体的头部
        sever_all_successors(last_block_in_chain);
        {
            IRBuilder builder;
            ir_builder_init(&builder, func);
            ir_builder_set_insertion_block_end(&builder, last_block_in_chain);
            ir_builder_create_br(&builder, (IRBasicBlock*)value_map_get(&iter_remap, (IRValue*)header, func->module->log_config));
        }

        // 更新 `value_remap`，为下一次克隆做准备
        value_map_merge(&value_remap, &iter_remap);
        last_block_in_chain = (IRBasicBlock*)value_map_get(&iter_remap, (IRValue*)latch, func->module->log_config);
    }
    
    // --- 2. 修改原始循环 ---
    
    // a. 将最后一个克隆副本的 latch 连接回原始的循环头
    sever_all_successors(last_block_in_chain);
    {
        IRBuilder builder;
        ir_builder_init(&builder, func);
        ir_builder_set_insertion_block_end(&builder, last_block_in_chain);
        ir_builder_create_br(&builder, header);
    }
    
    // b. 更新原始循环头的 PHI 节点
    for (IRInstruction* phi = header->head; phi && phi->opcode == IR_OP_PHI; phi = phi->next) {
        // 找到来自 latch 的入口
        for (IROperand* op = phi->operand_head; op; op = op->next_in_instr->next_in_instr) {
            if (op->next_in_instr->data.bb == latch) {
                // 将其值更新为最后一次展开产生的值
                change_operand_value(op, remap_value(&value_remap, op->data.value));
                // 将其来源块更新为最后一个克隆的 latch
                op->next_in_instr->data.bb = last_block_in_chain;
                break;
            }
        }
    }
    
    // c. 修改归纳变量的步进值
    IRBuilder builder;
    ir_builder_init(&builder, func);
    ir_builder_set_insertion_point(&builder, iv_info->update_instr);
    
    IRValue* factor_val = ir_builder_create_const_int(&builder, unroll_factor);
    IRValue* new_step = ir_builder_create_mul(&builder, iv_info->step_val, factor_val, "step.unrolled")->dest;
    
    // 创建新的更新指令，替换旧的
    IRValue* old_iv_val = iv_info->phi->dest;
    IRValue* new_iv_update_val = ir_builder_create_add(&builder, old_iv_val, new_step, "iv.unrolled")->dest;
    
    replace_all_uses_with(NULL, iv_info->update_instr->dest, new_iv_update_val);
    erase_instruction(iv_info->update_instr); // 删除旧的 i = i + 1
}

/**
 * @brief 在循环中查找一个规范化的归纳变量。
 * @details
 *  这个实现专注于识别 `while (i < N)` 这种由 SysY 代码生成的典型模式。
 *  它寻找一个 PHI 节点，其值由一个常量和一个循环内的加法/减法指令决定。
 * @param loop 要分析的循环。
 * @param iv_info 用于存储分析结果的结构体。
 */
void find_canonical_iv(Loop* loop, CanonicalIVInfo* iv_info) {
    memset(iv_info, 0, sizeof(CanonicalIVInfo));
    IRBasicBlock* header = loop->header;
    IRBasicBlock* preheader = loop->preheader;
    if (!preheader || loop->num_back_edges != 1) {
        iv_info->is_canonical = false;
        return;
    }
    IRBasicBlock* latch = loop->back_edges[0];
    for (IRInstruction* phi = header->head; phi && phi->opcode == IR_OP_PHI; phi = phi->next) {
        // 遍历所有入口，找 preheader 和 latch
        IRValue* start_val = NULL;
        IRValue* recur_val = NULL;
        for (IROperand* op = phi->operand_head; op && op->next_in_instr; op = op->next_in_instr->next_in_instr) {
            IRValue* val = op->data.value;
            IRBasicBlock* from = op->next_in_instr->data.bb;
            if (from == preheader) start_val = val;
            else if (from == latch) recur_val = val;
        }
        if (!start_val || !recur_val) continue;
        if (!start_val->is_constant || !recur_val->def_instr) continue;
        IRInstruction* update_instr = recur_val->def_instr;
        if (update_instr->opcode != IR_OP_ADD && update_instr->opcode != IR_OP_SUB) continue;
        if (update_instr->parent != latch) continue;
        IRValue* op1 = update_instr->operand_head->data.value;
        IRValue* op2 = update_instr->operand_head->next_in_instr->data.value;
        IRValue* step_val = NULL;
        if (op1 == phi->dest) step_val = op2;
        else if (op2 == phi->dest) step_val = op1;
        else continue;
        if (!step_val->is_constant) continue;
        IRInstruction* exit_branch = latch->tail;
        if (exit_branch->opcode != IR_OP_BR || exit_branch->num_operands != 3) continue;
        IRInstruction* cmp = exit_branch->operand_head->data.value->def_instr;
        if (!cmp || cmp->opcode != IR_OP_ICMP) continue;
        IRValue* cmp_lhs = cmp->operand_head->data.value;
        IRValue* cmp_rhs = cmp->operand_head->next_in_instr->data.value;
        IRValue* limit_val = NULL;
        if (cmp_lhs == phi->dest) limit_val = cmp_rhs;
        else if (cmp_rhs == phi->dest) limit_val = cmp_lhs;
        else continue;
        if (!limit_val->is_constant) continue;
        long long start = start_val->int_val;
        long long limit = limit_val->int_val;
        long long step = step_val->int_val;
        if (update_instr->opcode == IR_OP_SUB) step = -step;
        long long trip_count = -1;
        if (step > 0 && strcmp(cmp->opcode_cond, "slt") == 0) {
            if (limit <= start) trip_count = 0;
            else trip_count = (limit - 1 - start) / step + 1;
        } else if (step < 0 && strcmp(cmp->opcode_cond, "sgt") == 0) {
            if (limit >= start) trip_count = 0;
            else trip_count = (start - (limit + 1)) / (-step) + 1;
        } else {
            continue;
        }
        if (trip_count < 0 || trip_count > 1000000) {
            iv_info->is_canonical = false;
            return;
        }
        iv_info->is_canonical = true;
        iv_info->phi = phi;
        iv_info->cmp = cmp;
        iv_info->update_instr = update_instr;
        iv_info->start_val = start_val;
        iv_info->limit_val = limit_val;
        iv_info->step_val = step_val;
        iv_info->trip_count = (int)trip_count;
        return;
    }
    iv_info->is_canonical = false;
}

// 声明外部函数
extern IRInstruction* clone_instruction(IRInstruction* instr, MemoryPool* pool);
extern void remap_instruction_operands(IRInstruction* instr, ValueMap* value_remap);
extern void sever_all_successors(IRBasicBlock* bb);
extern void value_map_merge(ValueMap* dst, ValueMap* src);