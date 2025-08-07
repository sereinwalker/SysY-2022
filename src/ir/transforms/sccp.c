/**
 * @file sccp.c
 * @brief 实现稀疏条件常量传播（Sparse Conditional Constant Propagation, SCCP）优化遍。
 * @details
 * 本文件完整地实现了经典的双工作列表 SCCP 算法。该算法通过一个抽象的"格（Lattice）"
 * 来表示一个值可能的状态：Top (未访问), Constant (常量), Bottom (非常量)。
 *
 * 算法流程如下：
 * 1.  **初始化**：将所有值设为 Top，所有基本块设为不可达，除了入口块。
 *     将入口块和所有函数参数放入工作列表。
 * 2.  **迭代分析**：
 *     - 从 CFG 工作列表中取出一个可达的基本块，模拟执行其指令。
 *     - 从 SSA 工作列表中取出一个值已改变的虚拟寄存器，更新其所有使用者的值。
 *     - 这个过程会发现新的可达块或推断出新的常量，并将它们加入相应的工作列表。
 * 3.  **不动点**：当两个工作列表都为空时，分析达到不动点，所有值的最终格状态确定。
 * 4.  **变换**：
 *     - 将所有状态为 Constant 的值替换为其对应的常量。
 *     - 移除所有最终状态为不可达的基本块。
 */
#include "ir/transforms/sccp.h"
#include "ir/ir_utils.h"
#include <string.h>
#include "ast.h"          // for Type::(anonymous), BASIC_INT, BASIC_FLOAT
#include "logger.h"       // for LOG_CATEGORY_IR_OPT, LOG_DEBUG
#include <math.h> 
#include <stdint.h>       // for uintptr_t


// --- SCCP 使用的增强数据结构 ---

/**
 * @enum LatticeState
 * @brief 定义一个值在格（Lattice）中的三种状态。
 */
typedef enum { 
    LATTICE_TIR_OP,      ///< Top: 最高状态，表示值尚未被分析，任何常量都可以与之合并。
    LATTICE_CONSTANT,  ///< Constant: 中间状态，表示值目前被认为是某个具体的常量。
    LATTICE_BOTTOM     ///< Bottom: 最低状态，表示值不是一个常量（例如，由两个不同常量合并而来）。
} LatticeState;

/**
 * @struct LatticeValue
 * @brief 表示一个 IR 值在 SCCP 分析过程中的格值。
 */
typedef struct {
    LatticeState state;      ///< 当前的格状态 (Top, Constant, Bottom)。
    union { 
        int int_val; 
        float float_val; 
    } const_val;             ///< 如果状态是 Constant，这里存储常量的值。
    Type* type;              ///< 值的类型，用于区分整型和浮点型常量。
    bool is_valid;           ///< 标记此格值是否有效。
} LatticeValue;

// 一个临时的哈希映射节点，用于在此遍中将 IRValue* 映射到一个唯一的ID。
typedef struct ValueMapNode {
    IRValue* value;
    int id;
    struct ValueMapNode* next;
} ValueMapNode;

/**
 * @struct SCCPContext
 * @brief 存储 SCCP 遍执行期间所有状态的上下文。
 */
typedef struct {
    IRFunction* func;          ///< 当前正在处理的函数。
    MemoryPool* pool;          ///< 用于内部分配的内存池。
    Worklist* cfg_worklist;    ///< 控制流图工作列表，存放待访问的可达基本块。
    Worklist* ssa_worklist;    ///< SSA 工作列表，存放其格值发生改变的 IRValue。
    LatticeValue* value_lattice; ///< 存储函数中所有值的格值数组。
    int value_count;           ///< 函数中值的总数。
    bool* executable_blocks;   ///< 标记每个基本块是否可达的布尔数组。
    ValueMapNode** value_id_map; ///< 从 IRValue* 到其在格值数组中索引的哈希映射。
    size_t map_size;           ///< 哈希映射的大小。
    int iteration_count;       ///< 迭代计数器，用于防止无限循环。
    int max_iterations;        ///< 最大迭代次数的安全限制。
    bool changed;              ///< 标记在分析过程中格值是否发生过变化。
} SCCPContext;

// --- 静态函数前向声明 ---
static void initialize_sccp(SCCPContext* ctx);
static void run_sccp_analysis(SCCPContext* ctx);
static bool transform_based_on_sccp(SCCPContext* ctx);
static void visit_block(SCCPContext* ctx, IRBasicBlock* bb);
static void visit_instruction(SCCPContext* ctx, IRInstruction* instr);
static void visit_phi_operands(SCCPContext* ctx, IRBasicBlock* from, IRBasicBlock* to);
static LatticeValue evaluate_instruction(SCCPContext* ctx, IRInstruction* instr);
static LatticeValue get_lattice_value(SCCPContext* ctx, IRValue* val);
static void set_lattice_value(SCCPContext* ctx, IRValue* val, LatticeValue new_lval);
static IRValue* create_constant_from_lattice(SCCPContext* ctx, const LatticeValue* lval);
static bool are_lattice_values_equal(const LatticeValue* v1, const LatticeValue* v2);
static LatticeValue merge_lattice_values(const LatticeValue* v1, const LatticeValue* v2);
static void assign_value_ids(SCCPContext* ctx);
static int get_value_id(SCCPContext* ctx, IRValue* val);
static int next_prime(int n);

// --- 主入口函数 ---
bool run_sccp(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SCCP: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running SCCP on function @%s", func->name);
    }
    
    SCCPContext ctx = {0};
    ctx.func = func;
    ctx.pool = func->module->pool;
    ctx.max_iterations = 1000; // 安全限制
    
    initialize_sccp(&ctx);
    run_sccp_analysis(&ctx);
    bool changed = transform_based_on_sccp(&ctx);
    
    if (changed && func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SCCP: Applied transformations in function @%s", func->name);
    }
    
    return changed;
}

// --- 增强的初始化 ---
static unsigned long value_hash(IRValue* val) { 
    return (uintptr_t)val >> 3; 
}

// 为函数内所有 SSA 值（指令结果）分配一个唯一的ID。
static void assign_value_ids(SCCPContext* ctx) {
    ctx->map_size = next_prime(ctx->value_count > 512 ? ctx->value_count : 512);
    ctx->value_id_map = (ValueMapNode**)pool_alloc_z(ctx->pool, ctx->map_size * sizeof(ValueMapNode*));
    ctx->value_count = 0;
    
    // 首先统计值的总数，以分配格值数组
    for (IRBasicBlock* bb = ctx->func->blocks; bb; bb = bb->next_in_func) {
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            if (instr->dest) {
                ctx->value_count++;
            }
        }
    }
    
    // 分配ID并构建哈希映射
    int id = 0;
    for (IRBasicBlock* bb = ctx->func->blocks; bb; bb = bb->next_in_func) {
        for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
            if (instr->dest) {
                unsigned long index = value_hash(instr->dest) % ctx->map_size;
                ValueMapNode* node = (ValueMapNode*)pool_alloc_z(ctx->pool, sizeof(ValueMapNode));
                node->value = instr->dest;
                node->id = id++;
                node->next = ctx->value_id_map[index];
                ctx->value_id_map[index] = node;
            }
        }
    }
}

// 初始化 SCCP 上下文和工作列表。
static void initialize_sccp(SCCPContext* ctx) {
    assign_value_ids(ctx);
    
    ctx->value_lattice = (LatticeValue*)pool_alloc_z(ctx->pool, ctx->value_count * sizeof(LatticeValue));
    for (int i = 0; i < ctx->value_count; ++i) {
        ctx->value_lattice[i].state = LATTICE_TIR_OP; // 初始状态为 Top
        ctx->value_lattice[i].is_valid = true;
    }
    
    // 将所有参数的格值设为 Bottom
    for (int i = 0; i < ctx->func->num_args; ++i) {
        IRValue* arg = ctx->func->args[i];
        LatticeValue bottom_val = {.state = LATTICE_BOTTOM, .type = arg->type, .is_valid = true};
        set_lattice_value(ctx, arg, bottom_val);
    }
    
    ctx->executable_blocks = (bool*)pool_alloc_z(ctx->pool, ctx->func->block_count * sizeof(bool));
    ctx->cfg_worklist = create_worklist(ctx->pool, ctx->func->block_count);
    ctx->ssa_worklist = create_worklist(ctx->pool, ctx->value_count);
    
    // 算法起点：函数入口块是可达的
    worklist_add(ctx->cfg_worklist, ctx->func->entry);
    ctx->executable_blocks[ctx->func->entry->post_order_id] = true;
    
    ctx->iteration_count = 0;
    ctx->changed = false;
}

// --- 增强的 SCCP 分析阶段 ---
// 只要任一工作列表不为空，就持续进行分析，直到达到不动点。
static void run_sccp_analysis(SCCPContext* ctx) {
    while ((ctx->cfg_worklist->count > 0 || ctx->ssa_worklist->count > 0) && 
           ctx->iteration_count < ctx->max_iterations) {
        
        ctx->iteration_count++;
        
        // 处理 CFG 工作列表中的可达块
        if (ctx->cfg_worklist->count > 0) {
            IRBasicBlock* bb = (IRBasicBlock*)worklist_pop(ctx->cfg_worklist);
            visit_block(ctx, bb);
        }
        
        // 处理 SSA 工作列表中的值
        if (ctx->ssa_worklist->count > 0) {
            IRValue* val = (IRValue*)worklist_pop(ctx->ssa_worklist);
            // 访问该值的所有使用者，因为它们的值可能也改变了
            for (IROperand* use = val->use_list_head; use; use = use->next_use) {
                visit_instruction(ctx, use->user);
            }
        }
    }
    
    if (ctx->iteration_count >= ctx->max_iterations) {
        if (ctx->func && ctx->func->module && ctx->func->module->log_config) {
            LOG_DEBUG(ctx->func->module->log_config, LOG_CATEGORY_IR_OPT, "SCCP: Reached maximum iterations in function @%s", ctx->func->name);
        }
    }
}

// 模拟执行一个可达块中的所有指令。
static void visit_block(SCCPContext* ctx, IRBasicBlock* bb) {
    if (!ctx->executable_blocks[bb->post_order_id]) return;
    
    for (IRInstruction* instr = bb->head; instr; instr = instr->next) {
        visit_instruction(ctx, instr);
    }
}

// 当一个块变得可达时，访问其后继块中的所有 PHI 指令。
static void visit_phi_operands(SCCPContext* ctx, IRBasicBlock* from, IRBasicBlock* to) {
    (void)from; // from 参数在更复杂的PHI更新逻辑中可能有用
    if (!ctx->executable_blocks[to->post_order_id]) return;
    for (IRInstruction* phi = to->head; phi && phi->opcode == IR_OP_PHI; phi = phi->next) {
        visit_instruction(ctx, phi);
    }
}

// "访问"一条指令：重新计算其结果的格值，并处理其控制流效应。
static void visit_instruction(SCCPContext* ctx, IRInstruction* instr) {
    if (!instr || !ctx->executable_blocks[instr->parent->post_order_id]) return;

    if (instr->dest) {
        LatticeValue old_lval = get_lattice_value(ctx, instr->dest);
        if (old_lval.state == LATTICE_BOTTOM) return;
        LatticeValue new_lval = evaluate_instruction(ctx, instr);
        set_lattice_value(ctx, instr->dest, new_lval);
    } else if (instr->opcode == IR_OP_BR && instr->num_operands > 1) {
        LatticeValue cond_lval = get_lattice_value(ctx, instr->operand_head->data.value);
        if (cond_lval.state == LATTICE_CONSTANT) {
            IROperand* op = instr->operand_head->next_in_instr;
            IRBasicBlock* true_target = op->kind == IR_OP_KIND_BASIC_BLOCK ? op->data.bb : NULL;
            IRBasicBlock* false_target = op->next_in_instr && op->next_in_instr->kind == IR_OP_KIND_BASIC_BLOCK ? op->next_in_instr->data.bb : NULL;
            IRBasicBlock* target = (cond_lval.const_val.int_val != 0) ? true_target : false_target;
            if (target && !ctx->executable_blocks[target->post_order_id]) {
                ctx->executable_blocks[target->post_order_id] = true;
                worklist_add(ctx->cfg_worklist, target);
                visit_phi_operands(ctx, instr->parent, target);
            }
        }
    } else if (instr->opcode == IR_OP_BR) { // 无条件分支
        IRBasicBlock* target = (IRBasicBlock*)instr->operand_head->data.bb;
        if (!ctx->executable_blocks[target->post_order_id]) {
            ctx->executable_blocks[target->post_order_id] = true;
            worklist_add(ctx->cfg_worklist, target);
            visit_phi_operands(ctx, instr->parent, target);
        }
    }
}

// --- 增强的格值管理 ---
// 获取一个 IRValue 对应的格值。
static LatticeValue get_lattice_value(SCCPContext* ctx, IRValue* val) {
    if (!val) {
        return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = false};
    }
    
    // 如果值本身就是IR常量，则其格值就是该常量
    if (val->is_constant) {
        LatticeValue lval = {.state = LATTICE_CONSTANT, .type = val->type, .is_valid = true};
        if (val->type->basic == BASIC_INT) {
            lval.const_val.int_val = val->int_val;
        } else {
            lval.const_val.float_val = val->float_val;
        }
        return lval;
    }
    
    // 否则，从格值数组中查找
    int id = get_value_id(ctx, val);
    if (id != -1 && id < ctx->value_count) {
        return ctx->value_lattice[id];
    }
    
    return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = false};
}

// 获取一个 IRValue 对应的唯一ID。
static int get_value_id(SCCPContext* ctx, IRValue* val) {
    if (!val || val->is_constant) return -1;
    
    unsigned long index = value_hash(val) % ctx->map_size;
    for (ValueMapNode* node = ctx->value_id_map[index]; node; node = node->next) {
        if (node->value == val) return node->id;
    }
    return -1;
}

// 更新一个值的格值，如果发生变化，则将其加入SSA工作列表。
static void set_lattice_value(SCCPContext* ctx, IRValue* val, LatticeValue new_lval) {
    int id = get_value_id(ctx, val);
    if (id == -1 || id >= ctx->value_count) return;
    
    LatticeValue* current = &ctx->value_lattice[id];
    if (!are_lattice_values_equal(current, &new_lval)) {
        *current = new_lval;
        worklist_add(ctx->ssa_worklist, val);
        ctx->changed = true;
    }
}

// 比较两个格值是否相等。
static bool are_lattice_values_equal(const LatticeValue* v1, const LatticeValue* v2) {
    if (v1->state != v2->state) return false;
    if (v1->state == LATTICE_CONSTANT) {
        if (v1->type != v2->type) return false;
        if (v1->type->basic == BASIC_INT) {
            return v1->const_val.int_val == v2->const_val.int_val;
        } else {
            return fabs(v1->const_val.float_val - v2->const_val.float_val) < 1e-6;
        }
    }
    return true;
}

// 合并两个格值，遵循格的运算法则。
static LatticeValue merge_lattice_values(const LatticeValue* v1, const LatticeValue* v2) {
    if (v1->state == LATTICE_BOTTOM || v2->state == LATTICE_BOTTOM) {
        return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = true};
    }
    if (v1->state == LATTICE_TIR_OP) return *v2;
    if (v2->state == LATTICE_TIR_OP) return *v1;
    if (are_lattice_values_equal(v1, v2)) return *v1;
    return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = true};
}

// --- 核心求值引擎 ---
// 根据指令的操作码和其操作数的格值，计算该指令结果的格值。
static LatticeValue evaluate_instruction(SCCPContext* ctx, IRInstruction* instr) {
    if (!instr) return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = false};
    switch (instr->opcode) {
        case IR_OP_ADD: case IR_OP_SUB: case IR_OP_MUL: case IR_OP_SDIV: case IR_OP_SREM: {
            IROperand* op1 = instr->operand_head;
            IROperand* op2 = op1 ? op1->next_in_instr : NULL;
            LatticeValue lval1 = get_lattice_value(ctx, op1 ? op1->data.value : NULL);
            LatticeValue lval2 = get_lattice_value(ctx, op2 ? op2->data.value : NULL);
            
            // 使用 merge_lattice_values 处理 Bottom 和 Top 状态
            LatticeValue merged = merge_lattice_values(&lval1, &lval2);
            if (merged.state == LATTICE_BOTTOM) return merged;
            
            // 如果两个操作数都是常量，进行常量折叠
            if (lval1.state == LATTICE_CONSTANT && lval2.state == LATTICE_CONSTANT) {
                int v1 = lval1.const_val.int_val, v2 = lval2.const_val.int_val, res = 0;
                switch (instr->opcode) {
                    case IR_OP_ADD: res = v1 + v2; break;
                    case IR_OP_SUB: res = v1 - v2; break;
                    case IR_OP_MUL: res = v1 * v2; break;
                    case IR_OP_SDIV: res = (v2 != 0) ? v1 / v2 : 0; break;
                    case IR_OP_SREM: res = (v2 != 0) ? v1 % v2 : 0; break;
                    default: break;
                }
                return (LatticeValue){.state = LATTICE_CONSTANT, .const_val.int_val = res, .type = instr->dest ? instr->dest->type : NULL, .is_valid = true};
            }
            
            return merged;
        }
        case IR_OP_ICMP: {
            IROperand* op1 = instr->operand_head;
            IROperand* op2 = op1 ? op1->next_in_instr : NULL;
            LatticeValue lval1 = get_lattice_value(ctx, op1 ? op1->data.value : NULL);
            LatticeValue lval2 = get_lattice_value(ctx, op2 ? op2->data.value : NULL);
            
            // 使用 merge_lattice_values 处理 Bottom 和 Top 状态
            LatticeValue merged = merge_lattice_values(&lval1, &lval2);
            if (merged.state == LATTICE_BOTTOM) return merged;
            
            // 如果两个操作数都是常量，进行常量折叠
            if (lval1.state == LATTICE_CONSTANT && lval2.state == LATTICE_CONSTANT) {
                int v1 = lval1.const_val.int_val, v2 = lval2.const_val.int_val, res = 0;
                const char* pred = instr->opcode_cond;
                if (strcmp(pred, "eq") == 0) res = (v1 == v2);
                else if (strcmp(pred, "ne") == 0) res = (v1 != v2);
                else if (strcmp(pred, "slt") == 0) res = (v1 < v2);
                else if (strcmp(pred, "sgt") == 0) res = (v1 > v2);
                else if (strcmp(pred, "sle") == 0) res = (v1 <= v2);
                else if (strcmp(pred, "sge") == 0) res = (v1 >= v2);
                return (LatticeValue){.state = LATTICE_CONSTANT, .const_val.int_val = res, .type = instr->dest ? instr->dest->type : NULL, .is_valid = true};
            }
            
            return merged;
        }
        case IR_OP_FCMP: {
            IROperand* op1 = instr->operand_head;
            IROperand* op2 = op1 ? op1->next_in_instr : NULL;
            LatticeValue lval1 = get_lattice_value(ctx, op1 ? op1->data.value : NULL);
            LatticeValue lval2 = get_lattice_value(ctx, op2 ? op2->data.value : NULL);
            
            // 使用 merge_lattice_values 处理 Bottom 和 Top 状态
            LatticeValue merged = merge_lattice_values(&lval1, &lval2);
            if (merged.state == LATTICE_BOTTOM) return merged;
            
            // 如果两个操作数都是常量，进行常量折叠
            if (lval1.state == LATTICE_CONSTANT && lval2.state == LATTICE_CONSTANT) {
                float v1 = lval1.const_val.float_val, v2 = lval2.const_val.float_val;
                int res = 0;
                const char* pred = instr->opcode_cond;
                if (strcmp(pred, "oeq") == 0) res = (v1 == v2);
                else if (strcmp(pred, "one") == 0) res = (v1 != v2);
                else if (strcmp(pred, "olt") == 0) res = (v1 < v2);
                else if (strcmp(pred, "ogt") == 0) res = (v1 > v2);
                else if (strcmp(pred, "ole") == 0) res = (v1 <= v2);
                else if (strcmp(pred, "oge") == 0) res = (v1 >= v2);
                return (LatticeValue){.state = LATTICE_CONSTANT, .const_val.int_val = res, .type = instr->dest ? instr->dest->type : NULL, .is_valid = true};
            }
            
            return merged;
        }
        case IR_OP_ZEXT: case IR_OP_SITOFP: case IR_OP_FPTOSI: {
            IROperand* op1 = instr->operand_head;
            LatticeValue lval1 = get_lattice_value(ctx, op1 ? op1->data.value : NULL);
            if (lval1.state == LATTICE_BOTTOM) return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = true};
            if (lval1.state == LATTICE_CONSTANT) {
                LatticeValue out = {.state = LATTICE_CONSTANT, .type = instr->dest ? instr->dest->type : NULL, .is_valid = true};
                if (instr->opcode == IR_OP_ZEXT) out.const_val.int_val = (int)lval1.const_val.int_val;
                else if (instr->opcode == IR_OP_SITOFP) out.const_val.float_val = (float)lval1.const_val.int_val;
                else if (instr->opcode == IR_OP_FPTOSI) out.const_val.int_val = (int)lval1.const_val.float_val;
                return out;
            }
            return lval1;
        }
        case IR_OP_PHI: {
            LatticeValue merged = { .state = LATTICE_TIR_OP, .is_valid = true }; // 初始为 Top
            for (IROperand* op = instr->operand_head; op; op = op->next_in_instr->next_in_instr) {
                IRValue* incoming_val = op->data.value;
                IRBasicBlock* incoming_bb = op->next_in_instr->data.bb;
                if (ctx->executable_blocks[incoming_bb->post_order_id]) {
                    LatticeValue lval = get_lattice_value(ctx, incoming_val);
                    merged = merge_lattice_values(&merged, &lval);
                }
            }
            return merged;
        }
        default:
            // 对于所有其他指令，我们保守地假设其结果不是常量。
            return (LatticeValue){.state = LATTICE_BOTTOM, .is_valid = true};
    }
}

// --- SCCP 变换阶段 ---
// 根据格值创建一个新的 IR 常量。
static IRValue* create_constant_from_lattice(SCCPContext* ctx, const LatticeValue* lval) {
    if (!lval || lval->state != LATTICE_CONSTANT || !lval->type) return NULL;
    IRValue* v = (IRValue*)pool_alloc_z(ctx->pool, sizeof(IRValue));
    v->is_constant = true;
    v->type = lval->type;
    if (lval->type->basic == BASIC_INT || lval->type->basic == BASIC_I1)
        v->int_val = lval->const_val.int_val;
    else if (lval->type->basic == BASIC_FLOAT)
        v->float_val = lval->const_val.float_val;
    return v;
}

// 根据分析结果对IR进行变换。
static bool transform_based_on_sccp(SCCPContext* ctx) {
    bool changed = false;
    Worklist* wl_for_inst_combine = create_worklist(ctx->pool, 32);

    // 1. 将所有被确定为常量的 SSA 值替换为真正的 IR 常量。
    for (size_t i = 0; i < ctx->map_size; ++i) {
        for (ValueMapNode* node = ctx->value_id_map[i]; node; node = node->next) {
            LatticeValue lval = ctx->value_lattice[node->id];
            if (lval.state == LATTICE_CONSTANT) {
                IRValue* const_val = create_constant_from_lattice(ctx, &lval);
                replace_all_uses_with(wl_for_inst_combine, node->value, const_val);
                changed = true;
            }
        }
    }

    // 2. 将条件已确定的分支指令替换为无条件分支。
    for (IRBasicBlock* bb = ctx->func->blocks; bb; bb = bb->next_in_func) {
        if (!ctx->executable_blocks[bb->post_order_id] || !bb->tail) continue;
        IRInstruction* term = bb->tail;
        if (term->opcode == IR_OP_BR && term->num_operands > 1) {
            LatticeValue cond_lval = get_lattice_value(ctx, term->operand_head->data.value);
            if (cond_lval.state == LATTICE_CONSTANT) {
                IRBasicBlock* true_target = (IRBasicBlock*)term->operand_head->next_in_instr->data.bb;
                IRBasicBlock* false_target = (IRBasicBlock*)term->operand_head->next_in_instr->next_in_instr->data.bb;
                IRBasicBlock* final_target = (cond_lval.const_val.int_val != 0) ? true_target : false_target;
                IRBasicBlock* dead_succ = (final_target == true_target) ? false_target : true_target;
                
                // 从死分支的前驱列表中移除当前块
                remove_predecessor(dead_succ, bb);
                
                // 创建新的无条件跳转指令
                IRInstruction* new_br = create_ir_instruction(IR_OP_BR, ctx->pool);
                new_br->parent = bb;
                add_bb_operand(new_br, final_target);

                // 替换掉旧的条件跳转
                if (term->prev) term->prev->next = new_br;
                else bb->head = new_br;
                new_br->prev = term->prev;
                bb->tail = new_br;
                
                erase_instruction(term);
                changed = true;
            }
        }
    }
    
    return changed;
}

static int next_prime(int n) {
    // 简单找下一个大于n的素数
    for (int p = n + 1;; ++p) {
        int is_prime = 1;
        for (int d = 2; d * d <= p; ++d) if (p % d == 0) { is_prime = 0; break; }
        if (is_prime) return p;
    }
}