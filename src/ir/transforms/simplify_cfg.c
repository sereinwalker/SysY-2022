/**
 * @file simplify_cfg.c
 * @brief 实现一个迭代式的控制流图（CFG）简化优化遍。
 * @details
 * 本文件实现了一系列经典的CFG清理和简化技术。它通过一个不动点迭代框架，
 * 反复执行多个子优化遍，直到CFG结构稳定为止。这些优化对于清理由前端生成
 * 或由其他优化遍（如SCCP、循环展开）产生的冗余或复杂的控制流至关重要。
 */
#include "ir/transforms/simplify_cfg.h"
#include "ir/ir_utils.h"
#include "ir/ir_builder.h"
#include "ir/analysis/cfg_builder.h"
#include "ir/analysis/dominators.h"
#include <string.h>
#include "ir/ir_data_structures.h"
#include "logger.h"                   // for LOG_CATEGORY_IR_OPT, LOG_DEBUG


// 缺失的外部函数声明（应在对应的头文件中提供）
extern void ir_builder_set_insertion_block_end(IRBuilder* builder, IRBasicBlock* bb);
extern IRValue* phi_get_incoming_value_for_block(IRInstruction* phi, IRBasicBlock* block);
extern void move_instructions_to_block_end(IRBasicBlock* from, IRBasicBlock* to);
extern void replace_all_uses_with_block(IRBasicBlock* from, IRBasicBlock* to);
extern void remove_block_from_function(IRBasicBlock* bb);

// --- 上下文结构体定义 ---

/**
 * @brief 存储 CFG 简化遍执行期间所需状态的上下文。
 */
typedef struct {
    IRFunction* func;              ///< 当前正在处理的函数
    IRBuilder builder;             ///< 用于创建新指令（如无条件跳转）的构建器
    bool changed_this_iteration;   ///< 标记当前一轮不动点迭代中是否发生了改变
    bool changed_overall;          ///< 标记整个优化过程中是否发生了任何改变
} SimplifyCFGContext;

// --- 各子优化遍的原型声明 ---
static bool simplify_constant_branches(SimplifyCFGContext* ctx);
static bool remove_unreachable_blocks(SimplifyCFGContext* ctx);
static bool merge_sequential_blocks(SimplifyCFGContext* ctx);
static bool thread_jumps(SimplifyCFGContext* ctx);

// --- 主入口函数 ---
bool run_simplify_cfg(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SimplifyCFG: No function or entry block");
        }
        return false;
    }
    
    SimplifyCFGContext ctx = {0};
    ctx.func = func;
    ir_builder_init(&ctx.builder, func);
    ctx.changed_overall = false;

    // 使用不动点迭代框架，确保所有简化机会都被发掘
    while (true) {
        ctx.changed_this_iteration = false;

        // 在每轮迭代开始时，重新构建CFG和支配树信息，确保后续遍在最新的数据上操作
        build_cfg(func);
        compute_dominators(func);

        // 按顺序执行各个子优化遍
        if (simplify_constant_branches(&ctx)) {
            // 如果常量折叠改变了CFG，立即重建CFG，以便后续遍能看到变化
            build_cfg(func);
            compute_dominators(func);
        }
        
        thread_jumps(&ctx);
        merge_sequential_blocks(&ctx);
        remove_unreachable_blocks(&ctx);

        if (ctx.changed_this_iteration) {
            ctx.changed_overall = true;
        } else {
            // 如果在一整轮迭代中没有任何改变，说明已达到不动点，可以退出循环
            break;
        }
    }

    if (ctx.changed_overall && func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SimplifyCFG applied transformations in @%s", func->name);
    }

    return ctx.changed_overall;
}


// --- 各子优化遍的实现 ---

/**
 * @brief 子优化：常量分支折叠。
 * @details 查找所有条件为常量的 `br` 指令，并将其替换为指向相应目标的无条件 `br` 指令。
 *          同时，更新CFG中断开的边。
 */
static bool simplify_constant_branches(SimplifyCFGContext* ctx) {
    bool changed_locally = false;
    
    // 遍历块列表的副本，因为原始列表可能在变换中被修改
    Worklist* blocks_to_check = create_worklist(ctx->func->module->pool, ctx->func->block_count);
    for (IRBasicBlock* bb = ctx->func->blocks; bb; bb = bb->next_in_func) {
        worklist_add(blocks_to_check, bb);
    }
    
    while (blocks_to_check->count > 0) {
        IRBasicBlock* bb = (IRBasicBlock*)worklist_pop(blocks_to_check);
        // 只关心以条件分支结尾的块
        if (!bb || !bb->tail || bb->tail->opcode != IR_OP_BR || bb->tail->num_operands <= 1) continue;

        IROperand* term_cond_op = bb->tail->operand_head;
        if (term_cond_op->kind != IR_OP_KIND_VALUE) continue;
        IRValue* cond = term_cond_op->data.value;
        if (!cond->is_constant) continue;

        changed_locally = true;
        ctx->changed_this_iteration = true;
        
        if (ctx->func->module && ctx->func->module->log_config) {
            LOG_DEBUG(ctx->func->module->log_config, LOG_CATEGORY_IR_OPT, "SimplifyCFG: Folding branch in %s", bb->label);
        }

        // 确定保留的分支和要移除的分支
        IRBasicBlock* true_dest = bb->tail->operand_head->next_in_instr->data.bb;
        IRBasicBlock* false_dest = bb->tail->operand_head->next_in_instr->next_in_instr->data.bb;
        
        IRBasicBlock* kept_dest = (cond->int_val != 0) ? true_dest : false_dest;
        IRBasicBlock* dead_dest = (cond->int_val != 0) ? false_dest : true_dest;
        
        // 更新CFG：从死分支的前驱中移除当前块，并从当前块的后继中移除死分支
        remove_predecessor(dead_dest, bb);
        remove_successor(bb, dead_dest);
        
        // 擦除旧的条件分支，并创建一个新的无条件分支
        erase_instruction(bb->tail);
        ir_builder_set_insertion_block_end(&ctx->builder, bb);
        ir_builder_create_br(&ctx->builder, kept_dest);
    }
    return changed_locally;
}

/**
 * @brief 子优化：不可达块消除。
 * @details 从入口块开始进行图遍历，标记所有可达的块。然后遍历函数中的所有块，
 *          移除所有未被标记为可达的块。
 */
static bool remove_unreachable_blocks(SimplifyCFGContext* ctx) {
    if (!ctx->func->entry) return false;
    
    BitSet* reachable = bitset_create(ctx->func->block_count, ctx->func->module->pool);
    Worklist* wl = create_worklist(ctx->func->module->pool, ctx->func->block_count);
    
    // 从入口块开始进行前向遍历
    worklist_add(wl, ctx->func->entry);
    bitset_add(reachable, ctx->func->entry->post_order_id, ctx->func->module->log_config);

    while (wl->count > 0) {
        IRBasicBlock* bb = (IRBasicBlock*)worklist_pop(wl);
        for (int i = 0; i < bb->num_successors; ++i) {
            IRBasicBlock* succ = bb->successors[i];
            if (!bitset_contains(reachable, succ->post_order_id)) {
                bitset_add(reachable, succ->post_order_id, ctx->func->module->log_config);
                worklist_add(wl, succ);
            }
        }
    }

    bool changed_locally = false;
    // 反向遍历块链表，直接删除不可达块
    for (IRBasicBlock* bb = ctx->func->tail; bb; ) {
        IRBasicBlock* prev = bb->prev_in_func;
        if (bb != ctx->func->entry && !bitset_contains(reachable, bb->post_order_id)) {
            if (!changed_locally) {
                changed_locally = true;
                ctx->changed_this_iteration = true;
                
                if (ctx->func->module && ctx->func->module->log_config) {
                    LOG_DEBUG(ctx->func->module->log_config, LOG_CATEGORY_IR_OPT, "SimplifyCFG: Removing unreachable block %s", bb->label);
                }
            }
            remove_block_from_function(bb);
        }
        bb = prev;
    }
    return changed_locally;
}

/**
 * @brief 子优化：跳转线程化。
 * @details 查找只包含一个无条件跳转的"跳板"块 B (`br %C`)。
 *          然后将所有指向 B 的块 A 的跳转目标直接修改为 C。
 *          增强的PHI处理：对于C中的PHI节点，将来自B的入口重写为来自A的入口。
 */
static bool thread_jumps(SimplifyCFGContext* ctx) {
    bool changed_locally = false;
    bool needs_restart = true;

    while (needs_restart) {
        needs_restart = false;
        Worklist* blocks_to_check = create_worklist(ctx->func->module->pool, ctx->func->block_count);
        for (IRBasicBlock* bb = ctx->func->blocks; bb; bb = bb->next_in_func) {
            worklist_add(blocks_to_check, bb);
        }

        while(blocks_to_check->count > 0) {
            IRBasicBlock* bb_b = (IRBasicBlock*)worklist_pop(blocks_to_check);
            // 查找"跳板"块：只有一个无条件跳转指令
            if (bb_b->head != bb_b->tail || !bb_b->head || bb_b->head->opcode != IR_OP_BR || bb_b->head->num_operands != 1) continue;
            if (bb_b == ctx->func->entry || bb_b->num_predecessors == 0) continue;

            IRBasicBlock* bb_c = bb_b->head->operand_head->data.bb;
            if (bb_c == bb_b) continue; // 忽略到自身的循环

            // 检查所有前驱是否可线程化（保持原有 can_thread 逻辑）
            bool can_thread = true;
            for (int i = 0; i < bb_b->num_predecessors; ++i) {
                IRBasicBlock* pred_a = bb_b->predecessors[i];
                for (IRInstruction* phi = bb_c->head; phi && phi->opcode == IR_OP_PHI; phi = phi->next) {
                    IRValue* val_from_b = phi_get_incoming_value_for_block(phi, bb_b);
                    if (val_from_b == NULL) {
                        IRValue* val_from_a = phi_get_incoming_value_for_block(phi, pred_a);
                        if (val_from_a == NULL) {
                            can_thread = false;
                            break;
                        }
                    }
                }
                if (!can_thread) break;
            }
            if (!can_thread) continue;

            if (ctx->func->module && ctx->func->module->log_config) {
                LOG_DEBUG(ctx->func->module->log_config, LOG_CATEGORY_IR_OPT, "SimplifyCFG: Threading jump through %s to %s", bb_b->label, bb_c->label);
            }

            // 重定向所有 B 的前驱，让它们直接跳转到 C，并用通用 PHI 修复函数修正 SSA 数据流
            while (bb_b->num_predecessors > 0) {
                IRBasicBlock* pred_a = bb_b->predecessors[0];
                redirect_edge(pred_a, bb_b, bb_c);
                // 通用 PHI 修复，保证 pred_a -> bb_c 的 SSA 正确性
                repair_phi_nodes_after_edge_redirect(bb_c, pred_a, bb_b);
            }

            changed_locally = true;
            ctx->changed_this_iteration = true;
            needs_restart = true;
            break;
        }
    }
    return changed_locally;
}

/**
 * @brief 子优化：合并顺序块。
 * @details 查找一个块 A，它只有一个后继 B，且 B 只有一个前驱 A。
 *          如果 B 不包含 PHI 指令，则可以将 B 的所有指令移动到 A 的末尾，并移除 B。
 */
static bool merge_sequential_blocks(SimplifyCFGContext* ctx) {
    bool changed_locally = false;
    bool needs_restart = true;
    
    while (needs_restart) {
        needs_restart = false;
        Worklist* blocks_to_check = create_worklist(ctx->func->module->pool, ctx->func->block_count);
        for (IRBasicBlock* bb = ctx->func->blocks; bb; bb = bb->next_in_func) {
            worklist_add(blocks_to_check, bb);
        }

        while(blocks_to_check->count > 0) {
            IRBasicBlock* bb_a = (IRBasicBlock*)worklist_pop(blocks_to_check);

            // A 必须以无条件跳转结尾，且只有一个后继
            if (!bb_a->tail || bb_a->tail->opcode != IR_OP_BR || bb_a->num_successors != 1) continue;
            
            IRBasicBlock* bb_b = bb_a->successors[0];
            
            // B 的唯一前驱必须是 A，且 B 不能是入口块
            if (bb_b == ctx->func->entry || bb_b->num_predecessors != 1) continue;
            
            // B 不能有 PHI 指令，因为合并后 PHI 指令会变得无效
            if (bb_b->head && bb_b->head->opcode == IR_OP_PHI) continue;

            if (ctx->func->module && ctx->func->module->log_config) {
                LOG_DEBUG(ctx->func->module->log_config, LOG_CATEGORY_IR_OPT, "SimplifyCFG: Merging block %s into %s", bb_b->label, bb_a->label);
            }
            
            // 1. 移除 A 的终结符指令
            erase_instruction(bb_a->tail);
            bb_a->tail = NULL;

            // 2. 将 B 的所有指令移动到 A 的末尾
            if (bb_b->head) {
                move_instructions_to_block_end(bb_b, bb_a);
            }

            // 3. 更新所有对 B 的引用，使其指向 A
            replace_all_uses_with_block(bb_b, bb_a);
            // 4. 从函数中彻底移除块 B
            remove_block_from_function(bb_b);

            changed_locally = true;
            ctx->changed_this_iteration = true;
            needs_restart = true;
            break;
        }
    }
    return changed_locally;
}