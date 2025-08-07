/**
 * @file cfg_builder.c
 * @brief 实现控制流图（Control Flow Graph, CFG）的构建遍。
 * @details
 * 本文件负责为函数构建其控制流图。CFG 是许多后续分析和优化的基础。
 * 实现采用了一个高效的多遍方法：
 * 1.  **后继计算与前驱计数**: 遍历所有基本块，根据其终结符指令确定后继块，
 *     并对每个后继块的前驱数量进行累加。
 * 2.  **前驱数组分配**: 根据第一遍的计数结果，为每个基本块分配大小正好的前驱数组。
 * 3.  **前驱数组填充**: 再次遍历所有块，利用已知的后继关系来填充前驱数组。
 * 这种方法避免了在构建过程中对前驱列表进行动态扩容，提高了效率。
 */
#include "ir/analysis/cfg_builder.h"
#include "logger.h"
#include <string.h>
#include <assert.h>
#include "ast.h"

// --- 主要的 CFG 构建函数 ---

/**
 * @brief 为一个函数构建控制流图。
 * @param func 要为其构建CFG的函数。
 */
void build_cfg(IRFunction* func) {
    if (!func || !func->entry) {
        return;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN, "Building CFG for function @%s", func->name);
    }

    assert(func->module && func->module->pool);
    MemoryPool* pool = func->module->pool;
    
    // --- Pass 1: 初始化所有CFG链接并计算前驱数量 ---
    // 这个循环也清除了之前运行可能残留的旧CFG数据。
    for (IRBasicBlock* block = func->blocks; block; block = block->next_in_func) {
        block->num_successors = 0;
        block->successors = NULL;
        block->num_predecessors = 0; // 重置计数器
        block->predecessors = NULL;
    }

    // 这个循环计算后继列表，并为每个块的前驱数量计数。
    for (IRBasicBlock* block = func->blocks; block; block = block->next_in_func) {
        IRInstruction* term = block->tail;
        if (!term) {
            LOG_WARN(func->module->log_config, LOG_CATEGORY_IR_GEN, "Block %s in function @%s has no terminator instruction.", block->label, func->name);
            continue;
        }

        if (term->opcode == IR_OP_BR) {
            if (term->num_operands > 1) { // 条件分支 (cond, true_dest, false_dest)
                block->num_successors = 2;
                block->successors = pool_alloc(pool, 2 * sizeof(IRBasicBlock*));
                
                // 正确访问基本块指针
                IRBasicBlock* true_succ = term->operand_head->next_in_instr->data.bb;
                IRBasicBlock* false_succ = term->operand_head->next_in_instr->next_in_instr->data.bb;
                
                assert(true_succ && false_succ);
                block->successors[0] = true_succ;
                block->successors[1] = false_succ;
                
                // 增加其后继块的前驱计数。
                true_succ->num_predecessors++;
                false_succ->num_predecessors++;
            } else { // 无条件分支 (dest)
                block->num_successors = 1;
                block->successors = pool_alloc(pool, sizeof(IRBasicBlock*));
                
                IRBasicBlock* succ = term->operand_head->data.bb;

                assert(succ);
                block->successors[0] = succ;
                succ->num_predecessors++;
            }
        }
        // IR_OP_RET 没有后继，所以 num_successors 保持为 0。
    }

    // --- Pass 2: 根据计算出的数量为前驱列表分配内存 ---
    for (IRBasicBlock* block = func->blocks; block; block = block->next_in_func) {
        if (block->num_predecessors > 0) {
            block->predecessors = pool_alloc(pool, block->num_predecessors * sizeof(IRBasicBlock*));
            block->num_predecessors = 0; // 重置计数器，以便在下一遍中作为填充索引使用
        }
    }

    // --- Pass 3: 填充已分配的前驱列表 ---
    for (IRBasicBlock* block = func->blocks; block; block = block->next_in_func) {
        for (int i = 0; i < block->num_successors; ++i) {
            IRBasicBlock* succ = block->successors[i];
            // 将 'block' 作为 'succ' 的一个前驱添加进去。
            succ->predecessors[succ->num_predecessors++] = block;
        }
    }
    
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN, "CFG for @%s built successfully.", func->name);
}