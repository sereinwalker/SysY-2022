/**
 * @file tail_call_elim.c
 * @brief 实现尾调用消除（Tail Call Elimination）优化遍。
 * @details
 * 本文件实现了尾调用消除的核心逻辑。尾调用消除是一种重要的优化技术，它将尾递归调用
 * 转换为循环结构，从而避免函数调用栈的不断增长，提高程序的执行效率。
 *
 * 算法流程：
 * 1.  **识别尾调用**：遍历函数中的所有基本块，找到符合尾调用模式的call-ret指令对。
 *     尾调用要求call指令的结果直接被ret指令返回，且没有其他副作用。
 * 2.  **检查递归性**：确认尾调用是对当前函数的直接递归调用。
 * 3.  **执行转换**：将尾递归转换为循环结构：
 *     a. 创建新的循环头块，包含参数的PHI节点
 *     b. 将原始入口块重定向到循环头块
 *     c. 将尾递归调用替换为跳转到循环头块
 *     d. 更新所有对原始参数的使用为对PHI节点的使用
 */
#include "ir/transforms/tail_call_elim.h"
#include "ir/analysis/cfg_builder.h"
#include "ir/transforms/simplify_cfg.h"
#include "ir/ir_utils.h"
#include "ir/ir_builder.h"
#include <string.h>
#include "logger.h"                      // for LOG_CATEGORY_IR_OPT, LOG_DEBUG

// --- 静态函数声明 ---
static bool is_tail_call_pattern(IRInstruction* call_instr, IRInstruction* ret_instr);
static bool is_direct_recursive_call(IRInstruction* call_instr, IRFunction* func);
static bool eliminate_tail_call(IRInstruction* call_instr);

// --- 主入口函数 ---
bool run_tail_call_elim(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "TCE: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running TCE on function @%s", func->name);
    }
    
    bool changed = false;
    
    // 遍历所有基本块，寻找尾调用模式
    for (IRBasicBlock* bb = func->blocks; bb; bb = bb->next_in_func) {
        // 尾调用必须发生在以ret指令结尾的块中
        if (!bb->tail || bb->tail->opcode != IR_OP_RET) continue;
        
        IRInstruction* ret_instr = bb->tail;
        IRInstruction* call_instr = ret_instr->prev;
        
        if (!call_instr || call_instr->opcode != IR_OP_CALL) continue;
        
        // 检查是否为尾调用模式
        if (is_tail_call_pattern(call_instr, ret_instr) && 
            is_direct_recursive_call(call_instr, func)) {
            if (eliminate_tail_call(call_instr)) {
                changed = true;
                // 由于CFG已改变，建议重新开始遍历
                break;
            }
        }
    }
    
    if (changed) {
        // 尾调用消除会创建直接的br指令，并可能使某些块变得不可达
        // 在简化之前需要重建CFG
        build_cfg(func);
        run_simplify_cfg(func);
        if (func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "TCE: Applied transformations in function @%s", func->name);
        }
    }
    
    return changed;
}

/**
 * @brief 检查一个call-ret指令对是否构成严格的尾调用模式。
 * @details 严格的尾调用要求call指令的结果直接被ret指令返回（或两者都是void）。
 */
static bool is_tail_call_pattern(IRInstruction* call_instr, IRInstruction* ret_instr) {
    if (!call_instr || !ret_instr || 
        call_instr->opcode != IR_OP_CALL || ret_instr->opcode != IR_OP_RET) {
        return false;
    }
    
    // call和ret必须是连续的指令
    if (call_instr->next != ret_instr) {
        return false;
    }
    
    // 检查返回值是否匹配
    if (ret_instr->num_operands == 1) {
        // 非void返回：ret的值必须就是call的结果
        if (ret_instr->operand_head->data.value != call_instr->dest) {
            return false;
        }
    } else {
        // void返回：call也必须没有返回值
        if (call_instr->dest != NULL) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief 检查一个调用是否为对当前函数的直接递归调用。
 */
static bool is_direct_recursive_call(IRInstruction* call_instr, IRFunction* func) {
    if (!call_instr || call_instr->opcode != IR_OP_CALL || !call_instr->operand_head) {
        return false;
    }
    
    IRValue* callee_val = call_instr->operand_head->data.value;
    if (!callee_val || !callee_val->name || !func->name) {
        return false;
    }
    
    // 通过比较函数名来判断
    return strcmp(callee_val->name, func->name) == 0;
}

/**
 * @brief 执行消除尾递归的核心变换。
 * @details 采用标准方法：在函数入口创建PHI节点，将尾调用替换为跳转
 */
static bool eliminate_tail_call(IRInstruction* call_instr) {
    if (!call_instr || call_instr->opcode != IR_OP_CALL) {
        return false;
    }
    
    IRBasicBlock* tail_block = call_instr->parent;
    IRFunction* func = tail_block->parent;
    IRBasicBlock* entry_block = func->entry;
    IRInstruction* ret_instr = call_instr->next;
    
    if (!tail_block || !func || !entry_block || !ret_instr || ret_instr->opcode != IR_OP_RET) {
        return false;
    }
    
    if (!is_tail_call_pattern(call_instr, ret_instr) || !is_direct_recursive_call(call_instr, func)) {
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, 
                 "TCE: Eliminating tail call in block %s of function @%s", 
                 tail_block->label, func->name);
    }
    
    // 创建IRBuilder
    IRBuilder builder;
    ir_builder_init(&builder, func);
    
    // 1. 在函数入口为每个参数创建PHI节点
    ValueMap arg_remap;
    value_map_init(&arg_remap, func->module->pool);
    
    IROperand* arg_op = call_instr->operand_head->next_in_instr; // 跳过callee
    for (int i = 0; i < func->num_args; ++i) {
        IRValue* original_arg = func->args[i];
        // 在入口块创建PHI节点
        ir_builder_set_insertion_block_start(&builder, entry_block);
        IRInstruction* phi = ir_builder_create_phi(&builder, original_arg->type, original_arg->name);
        
        // 记录映射：原始参数 -> 新的PHI节点
        value_map_put(&arg_remap, original_arg, phi->dest, func->module->log_config);
        
        arg_op = arg_op->next_in_instr;
    }
    
    // 2. 遍历所有外部前驱，填充PHI的入口
    for (int i = 0; i < entry_block->num_predecessors; ++i) {
        IRBasicBlock* pred = entry_block->predecessors[i];
        // 为每个PHI节点添加来自外部调用者的入口
        for (IRInstruction* instr = entry_block->head; instr && instr->opcode == IR_OP_PHI; instr = instr->next) {
            // 找到对应的原始参数
            IRValue* original_arg = NULL;
            for (int j = 0; j < func->num_args; ++j) {
                IRValue* mapped_val = value_map_get(&arg_remap, func->args[j], func->module->log_config);
                if (mapped_val == instr->dest) {
                    original_arg = func->args[j];
                    break;
                }
            }
            if (original_arg) {
                ir_phi_add_incoming(instr, original_arg, pred);
            }
        }
    }
    
    // 3. 添加来自尾递归块的PHI入口
    arg_op = call_instr->operand_head->next_in_instr; // 重新获取参数
    for (int i = 0; i < func->num_args; ++i) {
        IRValue* original_arg = func->args[i];
        IRValue* recursive_arg = arg_op->data.value;
        IRValue* mapped_val = value_map_get(&arg_remap, original_arg, func->module->log_config);
        
        // 找到对应的PHI节点
        for (IRInstruction* instr = entry_block->head; instr && instr->opcode == IR_OP_PHI; instr = instr->next) {
            if (instr->dest == mapped_val) {
                ir_phi_add_incoming(instr, recursive_arg, tail_block);
                break;
            }
        }
        
        arg_op = arg_op->next_in_instr;
    }
    
    // 4. 替换函数体内所有对原始参数的使用
    for (IRBasicBlock* bb = func->blocks; bb; bb = bb->next_in_func) {
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            // 跳过我们刚刚创建的PHI节点，避免自引用
            if (bb == entry_block && instr->opcode == IR_OP_PHI) {
                continue;
            }
            
            // 重映射指令的操作数
            for (IROperand* op = instr->operand_head; op; op = op->next_in_instr) {
                if (op->kind == IR_OP_KIND_VALUE) {
                    IRValue* mapped_val = value_map_get(&arg_remap, op->data.value, func->module->log_config);
                    if (mapped_val) {
                        op->data.value = mapped_val;
                    }
                }
            }
        }
    }
    
    // 5. 删除旧的call和ret指令
    erase_instruction(call_instr);
    erase_instruction(ret_instr);
    
    // 6. 将tail_block的跳转目标重定向到entry_block
    ir_builder_set_insertion_block_end(&builder, tail_block);
    ir_builder_create_br(&builder, entry_block);
    
    // 7. 更新CFG
    add_successor(tail_block, entry_block);
    add_predecessor(entry_block, tail_block);
    
    return true;
}