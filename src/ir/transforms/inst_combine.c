/**
 * @file inst_combine.c
 * @brief 实现一个基于工作列表和访问者模式的指令合并优化遍。
 * @details
 * 本文件实现了指令合并的核心逻辑。它采用一个工作列表（Worklist）来驱动优化，
 * 确保只有可能被简化的指令才会被处理，避免了对整个函数进行不必要的重复扫描。
 *
 * 对于每种可简化的指令操作码，都有一个对应的 `visit_...` 函数。这种设计（访问者模式）
 * 使得添加新的优化模式变得简单，只需实现一个新的 `visit` 函数并将其添加到跳转表中即可。
 */
#include "ir/transforms/inst_combine.h"
#include "ir/ir_utils.h"
#include <string.h>
#include <assert.h>
#include "ast.h"          // for create_basic_type, pool_alloc, BASIC_FLOAT
#include "logger.h"       // for LOG_CATEGORY_IR_OPT, LOG_DEBUG

// --- 访问者上下文与函数指针类型 ---

/**
 * @brief 存储指令合并遍执行期间所需状态的上下文。
 */
typedef struct InstCombineContext {
    Worklist* wl;           ///< 指向全局工作列表的指针，用于将被修改指令的使用者重新入队
    IRInstruction* instr;   ///< 当前正在访问的指令
    MemoryPool* pool;       ///< 用于创建新常量值的内存池
    // 为方便起见，预先提取操作数
    IRValue* op1; 
    IRValue* op2;
    // 为需要更多操作数的指令（如PHI或GEP）预留的字段
    IRValue* op3;
    IRValue* op4;
    // 标记指令是否在原地被修改，并需要重新入队进行进一步处理
    bool re_queue; 
} InstCombineContext;

// 所有 visit 函数都将共享此函数签名。
typedef IRValue* (*InstVisitFn)(InstCombineContext* ctx);

// --- 访问者函数的前向声明 ---
static IRValue* visit_add(InstCombineContext* ctx);
static IRValue* visit_sub(InstCombineContext* ctx);
static IRValue* visit_mul(InstCombineContext* ctx);
static IRValue* visit_shl(InstCombineContext* ctx);
static IRValue* visit_sdiv(InstCombineContext* ctx);
static IRValue* visit_srem(InstCombineContext* ctx);
static IRValue* visit_ashr(InstCombineContext* ctx);
static IRValue* visit_and(InstCombineContext* ctx);
static IRValue* visit_fadd(InstCombineContext* ctx);
static IRValue* visit_fsub(InstCombineContext* ctx);
static IRValue* visit_fmul(InstCombineContext* ctx);
static IRValue* visit_icmp(InstCombineContext* ctx);
static IRValue* visit_fcmp(InstCombineContext* ctx);
static IRValue* visit_phi(InstCombineContext* ctx);
static IRValue* visit_gep(InstCombineContext* ctx);
static IRValue* visit_unhandled(InstCombineContext* ctx);
static IRValue* visit_fdiv(InstCombineContext* ctx);

// --- 访问者跳转表 ---
// 这是一个函数指针数组，通过指令的操作码直接索引到对应的处理函数。
static InstVisitFn visit_fn_table[IR_OP_UNKNOWN + 1] = {
    [IR_OP_ADD]  = visit_add,  [IR_OP_SUB]  = visit_sub,  [IR_OP_MUL]  = visit_mul,
    [IR_OP_SDIV] = visit_sdiv, [IR_OP_SREM] = visit_srem, [IR_OP_FADD] = visit_fadd,
    [IR_OP_FSUB] = visit_fsub, [IR_OP_FMUL] = visit_fmul, [IR_OP_SHL]  = visit_shl,
    [IR_OP_ASHR] = visit_ashr, [IR_OP_AND]  = visit_and,  [IR_OP_ICMP] = visit_icmp,
    [IR_OP_FCMP] = visit_fcmp, [IR_OP_PHI]  = visit_phi,  [IR_OP_GETELEMENTPTR] = visit_gep,
    [IR_OP_FDIV] = visit_fdiv,
};

// --- 本地辅助函数的声明 ---
static IRValue* create_const_int(MemoryPool* pool, int value);
static IRValue* create_const_float(MemoryPool* pool, float value);
static bool is_power_of_two(int n, int* log_val);

// --- 主入口函数 ---
bool run_inst_combine(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "InstCombine: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running InstCombine on function @%s", func->name);
    }
    
    bool changed_overall = false;
    MemoryPool* pool = func->module->pool;
    Worklist* wl = create_worklist(pool, func->block_count * 10);

    assert(func->reverse_post_order != NULL && "Reverse Post-Order not available for InstCombine!");
    
    // 初始填充工作列表：将函数中的所有指令加入
    for (int i = 0; i < func->block_count; ++i) {
        IRBasicBlock* bb = func->reverse_post_order[i];
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            worklist_add(wl, instr);
        }
    }

    while (wl->count > 0) {
        IRInstruction* instr = (IRInstruction*)worklist_pop(wl);
        if (instr->opcode == IR_OP_UNKNOWN) continue; // 已被处理和移除的指令

        InstVisitFn visit_fn = visit_fn_table[instr->opcode];
        if (!visit_fn) {
            visit_fn = visit_unhandled; // 如果没有专门的处理函数，则使用默认函数
        }

        // 准备访问者上下文
        InstCombineContext ctx = { .wl = wl, .instr = instr, .pool = pool, .re_queue = false };
        if (instr->num_operands > 0) ctx.op1 = instr->operand_head->data.value;
        if (instr->num_operands > 1) ctx.op2 = instr->operand_head->next_in_instr->data.value;
        if (instr->num_operands > 2) ctx.op3 = instr->operand_head->next_in_instr->next_in_instr->data.value;
        if (instr->num_operands > 3) ctx.op4 = instr->operand_head->next_in_instr->next_in_instr->next_in_instr->data.value;

        // 调用对应的 visit 函数进行处理
        IRValue* new_val = visit_fn(&ctx);

        if (new_val) {
            // 情况1：指令被简化为一个新的值（通常是常量）
            if (instr->dest) {
                // 将原指令的所有使用者都替换为这个新值
                replace_all_uses_with(wl, instr->dest, new_val);
            }
            // 移除已被替换的旧指令
            erase_instruction(instr);
            changed_overall = true;
        } else if (ctx.re_queue) {
            // 情况2：指令在原地被修改（例如 `x-c` 变为 `x+(-c)`），需要重新入队以进行进一步的合并
            worklist_add(wl, (IRValue*)instr);
            changed_overall = true;
        }
    }
    
    if (changed_overall && func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "InstCombine: Applied transformations in function @%s", func->name);
    }
    
    return changed_overall;
}

// --- 访问者函数的实现 ---

// 默认处理函数：对未特殊处理的指令不执行任何操作。
static IRValue* visit_unhandled(InstCombineContext* ctx) {
    (void)ctx;
    return NULL;
}

// 处理 `add` 指令。
static IRValue* visit_add(InstCombineContext* ctx) {
    IRInstruction* instr = ctx->instr;
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;

    // 模式1：常量折叠 (e.g., add 2, 3 -> 5)
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_int(ctx->pool, lhs->int_val + rhs->int_val);
    }
    
    // 模式2：规范化，将常量操作数移动到右侧 (e.g., add 5, x -> add x, 5)
    if (lhs->is_constant && !rhs->is_constant) {
        change_operand_value(instr->operand_head, rhs);
        change_operand_value(instr->operand_head->next_in_instr, lhs);
        ctx->re_queue = true; // 修改后重新入队
        return NULL;
    }
    
    // 模式3：代数化简 (e.g., x + 0 -> x)
    if (rhs->is_constant && rhs->int_val == 0) return lhs;
    
    // 模式4：抵消运算 (e.g., (x - y) + y -> x)
    if (lhs->def_instr && lhs->def_instr->opcode == IR_OP_SUB) {
        if (lhs->def_instr->operand_head->next_in_instr->data.value == rhs) {
            return lhs->def_instr->operand_head->data.value;
        }
    }
    // 模式5：抵消运算（交换律） (e.g., y + (x - y) -> x)
    if (rhs->def_instr && rhs->def_instr->opcode == IR_OP_SUB) {
        if (rhs->def_instr->operand_head->next_in_instr->data.value == lhs) {
             return rhs->def_instr->operand_head->data.value;
        }
    }

    return NULL;
}

// 处理 `sub` 指令。
static IRValue* visit_sub(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;

    // 模式1：常量折叠 (e.g., sub 5, 2 -> 3)
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_int(ctx->pool, lhs->int_val - rhs->int_val);
    }
    
    // 模式2：代数化简 (e.g., x - 0 -> x)
    if (rhs->is_constant && rhs->int_val == 0) return lhs;

    // 模式3：代数化简 (e.g., x - x -> 0)
    if (lhs == rhs) return create_const_int(ctx->pool, 0);

    // 模式4：将减法转换为加法 (e.g., x - c -> x + (-c))
    if (rhs->is_constant) {
        IRValue* neg_const = create_const_int(ctx->pool, -rhs->int_val);
        ctx->instr->opcode = IR_OP_ADD;
        change_operand_value(ctx->instr->operand_head->next_in_instr, neg_const);
        ctx->re_queue = true;
        return NULL;
    }
    
    return NULL;
}

// 处理 `mul` 指令。
static IRValue* visit_mul(InstCombineContext* ctx) {
    IRInstruction* instr = ctx->instr;
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;

    // 模式1：常量折叠
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_int(ctx->pool, lhs->int_val * rhs->int_val);
    }

    // 模式2：规范化，常量放右边
    if (lhs->is_constant && !rhs->is_constant) {
        change_operand_value(instr->operand_head, rhs);
        change_operand_value(instr->operand_head->next_in_instr, lhs);
        ctx->re_queue = true;
        return NULL;
    }

    // 模式3：代数化简
    if (rhs->is_constant) {
        int c = rhs->int_val;
        if (c == 0) return create_const_int(ctx->pool, 0); // x * 0 -> 0
        if (c == 1) return lhs; // x * 1 -> x
        if (c == -1) { // x * -1 -> 0 - x
            instr->opcode = IR_OP_SUB;
            change_operand_value(instr->operand_head, create_const_int(ctx->pool, 0));
            change_operand_value(instr->operand_head->next_in_instr, lhs);
            ctx->re_queue = true;
            return NULL;
        }
        
        // 模式4：强度削减 (e.g., x * (2^N) -> x << N)
        int log_val;
        if (is_power_of_two(c, &log_val)) {
            instr->opcode = IR_OP_SHL;
            change_operand_value(instr->operand_head->next_in_instr, create_const_int(ctx->pool, log_val));
            ctx->re_queue = true;
            return NULL;
        }
    }
    return NULL;
}

// 处理 `sdiv` 指令。
static IRValue* visit_sdiv(InstCombineContext* ctx) {
    IRInstruction* instr = ctx->instr;
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;

    // 除零是未定义行为，不进行折叠以避免改变程序语义
    if (rhs->is_constant && rhs->int_val == 0) return NULL;
    
    // 模式1：常量折叠
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_int(ctx->pool, lhs->int_val / rhs->int_val);
    }
    
    // 模式2：代数化简 (e.g., x / 1 -> x)
    if (rhs->is_constant && rhs->int_val == 1) return lhs;

    // 模式3：代数化简 (e.g., x / -1 -> 0 - x)
    if (rhs->is_constant && rhs->int_val == -1) {
        instr->opcode = IR_OP_SUB;
        change_operand_value(instr->operand_head, create_const_int(ctx->pool, 0));
        change_operand_value(instr->operand_head->next_in_instr, lhs);
        ctx->re_queue = true;
        return NULL;
    }
    
    // 模式4：代数化简 (e.g., 0 / x -> 0, if x != 0)
    if (lhs->is_constant && lhs->int_val == 0) {
        return create_const_int(ctx->pool, 0);
    }
    return NULL;
}

// 处理 `srem` 指令。
static IRValue* visit_srem(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    // 对零取模是未定义行为
    if (rhs->is_constant && rhs->int_val == 0) return NULL;

    // 模式1：常量折叠
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_int(ctx->pool, lhs->int_val % rhs->int_val);
    }
    
    // 模式2：代数化简 (e.g., x % 1 -> 0)
    if (rhs->is_constant && (rhs->int_val == 1 || rhs->int_val == -1)) {
        return create_const_int(ctx->pool, 0);
    }
    
    // 模式3：代数化简 (e.g., x % x -> 0)
    if (lhs == rhs) return create_const_int(ctx->pool, 0);

    return NULL;
}

// 处理 `icmp` 指令。
static IRValue* visit_icmp(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    const char* pred = ctx->instr->opcode_cond;

    if (lhs->is_constant && rhs->is_constant) {
        bool result = false;
        if (strcmp(pred, "eq") == 0) result = (lhs->int_val == rhs->int_val);
        else if (strcmp(pred, "ne") == 0) result = (lhs->int_val != rhs->int_val);
        else if (strcmp(pred, "slt") == 0) result = (lhs->int_val < rhs->int_val);
        else if (strcmp(pred, "sgt") == 0) result = (lhs->int_val > rhs->int_val);
        else if (strcmp(pred, "sle") == 0) result = (lhs->int_val <= rhs->int_val);
        else if (strcmp(pred, "sge") == 0) result = (lhs->int_val >= rhs->int_val);
        // 返回一个 i32 类型的常量 0 或 1
        return create_const_int(ctx->pool, result ? 1 : 0);
    }
    return NULL;
}

// 处理 `fadd` 指令。
static IRValue* visit_fadd(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_float(ctx->pool, lhs->float_val + rhs->float_val);
    }
    // fadd x, 0.0 -> x
    if (rhs->is_constant && rhs->float_val == 0.0f) return lhs;
    if (lhs->is_constant && lhs->float_val == 0.0f) return rhs;
    return NULL;
}

// 处理 `fsub` 指令。
static IRValue* visit_fsub(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_float(ctx->pool, lhs->float_val - rhs->float_val);
    }
    // fsub x, 0.0 -> x
    if (rhs->is_constant && rhs->float_val == 0.0f) return lhs;
    // fsub 0.0, x -> -x
    if (lhs->is_constant && lhs->float_val == 0.0f) {
        ctx->instr->opcode = IR_OP_FSUB;
        change_operand_value(ctx->instr->operand_head, create_const_float(ctx->pool, 0.0f));
        change_operand_value(ctx->instr->operand_head->next_in_instr, rhs);
        ctx->re_queue = true;
        return NULL;
    }
    return NULL;
}

// 处理 `fmul` 指令。
static IRValue* visit_fmul(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_float(ctx->pool, lhs->float_val * rhs->float_val);
    }
    // fmul x, 1.0 -> x
    if (rhs->is_constant && rhs->float_val == 1.0f) return lhs;
    if (lhs->is_constant && lhs->float_val == 1.0f) return rhs;
    // fmul x, 0.0 -> 0.0
    if ((rhs->is_constant && rhs->float_val == 0.0f) || (lhs->is_constant && lhs->float_val == 0.0f)) {
        return create_const_float(ctx->pool, 0.0f);
    }
    // fmul x, -1.0 -> fsub 0.0, x
    if (rhs->is_constant && rhs->float_val == -1.0f) {
        ctx->instr->opcode = IR_OP_FSUB;
        change_operand_value(ctx->instr->operand_head, create_const_float(ctx->pool, 0.0f));
        change_operand_value(ctx->instr->operand_head->next_in_instr, lhs);
        ctx->re_queue = true;
        return NULL;
    }
    if (lhs->is_constant && lhs->float_val == -1.0f) {
        ctx->instr->opcode = IR_OP_FSUB;
        change_operand_value(ctx->instr->operand_head, create_const_float(ctx->pool, 0.0f));
        change_operand_value(ctx->instr->operand_head->next_in_instr, rhs);
        ctx->re_queue = true;
        return NULL;
    }
    // fmul x, 2.0 -> fadd x, x
    if (rhs->is_constant && rhs->float_val == 2.0f) {
        ctx->instr->opcode = IR_OP_FADD;
        change_operand_value(ctx->instr->operand_head, lhs);
        change_operand_value(ctx->instr->operand_head->next_in_instr, lhs);
        ctx->re_queue = true;
        return NULL;
    }
    if (lhs->is_constant && lhs->float_val == 2.0f) {
        ctx->instr->opcode = IR_OP_FADD;
        change_operand_value(ctx->instr->operand_head, rhs);
        change_operand_value(ctx->instr->operand_head->next_in_instr, rhs);
        ctx->re_queue = true;
        return NULL;
    }
    return NULL;
}

// 处理 `fdiv` 指令。
static IRValue* visit_fdiv(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    
    // 避免除零
    if (rhs->is_constant && rhs->float_val == 0.0f) return NULL;
    
    // 模式1：常量折叠
    if (lhs->is_constant && rhs->is_constant) {
        return create_const_float(ctx->pool, lhs->float_val / rhs->float_val);
    }
    
    // 模式2：代数化简 (e.g., 0.0 / x -> 0.0, if x != 0)
    if (lhs->is_constant && lhs->float_val == 0.0f) {
        return create_const_float(ctx->pool, 0.0f);
    }
    
    // 模式3：代数化简 (e.g., x / 1.0 -> x)
    if (rhs->is_constant && rhs->float_val == 1.0f) return lhs;
    
    // 模式4：代数化简 (e.g., x / x -> 1.0, if x != 0)
    if (lhs == rhs) {
        return create_const_float(ctx->pool, 1.0f);
    }
    
    // 模式5：代数化简 (e.g., x / -1.0 -> fsub 0.0, x)
    if (rhs->is_constant && rhs->float_val == -1.0f) {
        ctx->instr->opcode = IR_OP_FSUB;
        change_operand_value(ctx->instr->operand_head, create_const_float(ctx->pool, 0.0f));
        change_operand_value(ctx->instr->operand_head->next_in_instr, lhs);
        ctx->re_queue = true;
        return NULL;
    }
    
    return NULL;
}

// 处理 `shl` 指令。
static IRValue* visit_shl(InstCombineContext* ctx) {
    (void)ctx;
    return NULL;
}

// 处理 `ashr` 指令。
static IRValue* visit_ashr(InstCombineContext* ctx) {
    (void)ctx;
    return NULL;
}

// 处理 `and` 指令。
static IRValue* visit_and(InstCombineContext* ctx) {
    (void)ctx;
    return NULL;
}

// 处理 `fcmp` 指令。
static IRValue* visit_fcmp(InstCombineContext* ctx) {
    IRValue *lhs = ctx->op1, *rhs = ctx->op2;
    const char* pred = ctx->instr->opcode_cond;

    if (lhs->is_constant && rhs->is_constant) {
        bool result = false;
        // 'o' 前缀代表 "ordered"，意味着如果任一操作数是 NaN，结果为 false
        // 'u' 前缀代表 "unordered"，意味着如果任一操作数是 NaN，结果为 true
        // SysY 没有 NaN，所以 oeq 和 ueq 等价。我们用 o 前缀。
        if (strcmp(pred, "oeq") == 0) result = (lhs->float_val == rhs->float_val);
        else if (strcmp(pred, "one") == 0) result = (lhs->float_val != rhs->float_val);
        else if (strcmp(pred, "olt") == 0) result = (lhs->float_val < rhs->float_val);
        else if (strcmp(pred, "ogt") == 0) result = (lhs->float_val > rhs->float_val);
        else if (strcmp(pred, "ole") == 0) result = (lhs->float_val <= rhs->float_val);
        else if (strcmp(pred, "oge") == 0) result = (lhs->float_val >= rhs->float_val);
        // 返回一个 i32 类型的常量 0 或 1
        return create_const_int(ctx->pool, result ? 1 : 0);
    }
    return NULL;
}

// 处理 `phi` 指令。
static IRValue* visit_phi(InstCombineContext* ctx) {
    IRInstruction* phi = ctx->instr;
    if (phi->num_operands == 0) return NULL;

    // 检查所有 incoming 值是否相同
    IRValue* first_val = phi->operand_head->data.value;
    bool all_same = true;
    for (IROperand* op = phi->operand_head; op; op = op->next_in_instr->next_in_instr) {
        if (op->data.value != first_val && op->data.value != phi->dest) {
            // 如果 incoming 值不同，或者不等于 PHI 本身（递归 PHI），则不能简化
            all_same = false;
            break;
        }
    }

    if (all_same) {
        // 所有 incoming 值都相同，用这个值替换 PHI
        return first_val;
    }
    
    // 还可以添加对单一前驱的检查
    // 如果一个基本块只有一个前驱，那么这个块中的所有 PHI 节点都是冗余的
    IRBasicBlock* bb = phi->parent;
    if (bb && bb->num_predecessors == 1) {
        // 找到唯一的 incoming 值
        for (IROperand* op = phi->operand_head; op; op = op->next_in_instr->next_in_instr) {
            if (op->next_in_instr && op->next_in_instr->data.bb == bb->predecessors[0]) {
                return op->data.value;
            }
        }
    }
    
    return NULL;
}

// 处理 `gep` 指令。
static IRValue* visit_gep(InstCombineContext* ctx) {
    (void)ctx;
    return NULL;
}

// --- 工具函数实现 ---

// 创建一个整型常量 IRValue。
static IRValue* create_const_int(MemoryPool* pool, int value) {
    IRValue* v = (IRValue*)pool_alloc(pool, sizeof(IRValue));
    memset(v, 0, sizeof(IRValue));
    v->is_constant = true;
    v->type = create_basic_type(BASIC_INT, false, pool);
    v->int_val = value;
    return v;
}

// 检查一个数是否为2的幂，如果是，则通过指针返回其对数值。
static bool is_power_of_two(int n, int* log_val) {
    if (n <= 0) return false;
    bool is_pow2 = (n & (n - 1)) == 0;
    if (is_pow2) {
        *log_val = 0;
        int temp = n;
        while ((temp & 1) == 0 && temp > 1) { // 修正循环条件
            temp >>= 1;
            (*log_val)++;
        }
    }
    return is_pow2;
}

// 创建一个浮点型常量 IRValue。
static IRValue* create_const_float(MemoryPool* pool, float value) {
    IRValue* v = (IRValue*)pool_alloc(pool, sizeof(IRValue));
    memset(v, 0, sizeof(IRValue));
    v->is_constant = true;
    v->type = create_basic_type(BASIC_FLOAT, false, pool);
    v->float_val = value;
    return v;
}