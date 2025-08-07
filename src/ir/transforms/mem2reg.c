/**
 * @file mem2reg.c
 * @brief 实现内存到寄存器（Mem2Reg）提升优化遍。
 * @details
 * 本文件实现了将栈分配变量（`alloca`）提升为 SSA 虚拟寄存器的核心算法。
 * 这是构建 SSA 形式的关键步骤，能极大地提升后续优化的效果。其主要流程包括：
 * 1.  分析函数中所有 `alloca` 指令，识别出可以安全提升的标量变量。
 * 2.  基于变量的定义点和支配树信息，计算出需要在哪些基本块中插入 PHI 节点。
 * 3.  在计算出的位置实际插入 PHI 节点。
 * 4.  通过递归遍历支配树，使用版本栈对变量进行重命名，将 `load` 和 `store`
 *     操作替换为对 SSA 值的直接引用。
 * 5.  清理掉已经被提升、不再需要的 `alloca` 指令。
 */
#include "ir/transforms/mem2reg.h"
#include "ir/ir_utils.h"
#include "ir/ir_builder.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "ast.h"            // for pool_alloc, Type::(anonymous union)::(ano...
#include "logger.h"         // for LOG_CATEGORY_IR_OPT, LOG_DEBUG, logger_co...


// --- 其他模块辅助函数的前向声明 ---
void ir_builder_set_insertion_block_start(IRBuilder* builder, IRBasicBlock* block);
void mark_instruction_for_removal(IRInstruction* instr);
void cleanup_removed_instructions(IRBasicBlock* block);

// --- Mem2Reg 的增强数据结构 ---

/**
 * @struct PromotableAlloca
 * @brief 存储一个可提升的 alloca 及其相关分析信息。
 */
typedef struct {
    IRInstruction* alloca_instr;        ///< 指向 alloca 指令本身
    IRValue* alloca_val;                ///< 指向 alloca 指令的结果值（即指针）
    BitSet* defining_blocks;            ///< 存放所有对该 alloca 进行 store 操作的基本块的集合
    BitSet* phi_placement_blocks;       ///< 存放需要为该 alloca 插入 PHI 节点的基本块的集合
} PromotableAlloca;

/**
 * @struct VersionStackNode
 * @brief 用于变量重命名算法中的版本栈的节点。
 */
typedef struct VersionStackNode {
    IRValue* value;                     ///< 当前作用域中变量的"最新版本"值
    struct VersionStackNode* next;      ///< 指向栈中下一个（旧版本）的节点
} VersionStackNode;

/**
 * @struct Mem2RegContext
 * @brief 在 Mem2Reg 遍的执行过程中维护所有状态的上下文。
 */
typedef struct {
    IRFunction* func;                   ///< 正在处理的函数
    MemoryPool* pool;                   ///< 用于分配内存的内存池
    PromotableAlloca* promotables;      ///< 可提升 alloca 的数组
    int promotable_count;               ///< 可提升 alloca 的数量
    IRBuilder builder;                  ///< 用于创建新指令（如PHI）的构建器
    IRValue* undef_val;                 ///< 代表"未定义"的特殊IRValue
} Mem2RegContext;

// --- 辅助函数原型声明 ---
static bool is_alloca_promotable(IRInstruction* alloca_instr);
static void analyze_allocas(Mem2RegContext* ctx);
static void compute_phi_placement(Mem2RegContext* ctx);
static void insert_phi_nodes(Mem2RegContext* ctx);
static void rename_variables(Mem2RegContext* ctx);
static void rename_recursive(IRBasicBlock* block, Mem2RegContext* ctx, VersionStackNode** stacks);
static void push_version(VersionStackNode** stack_top, IRValue* value, MemoryPool* pool);
static IRValue* top_version(VersionStackNode* stack_top);
static void pop_version(VersionStackNode** stack_top);

// --- 本地辅助函数：获取操作码名称 ---
static const char* get_opcode_name(Opcode opcode) {
    switch (opcode) {
        case IR_OP_RET: return "ret";
        case IR_OP_BR: return "br";
        case IR_OP_ADD: return "add";
        case IR_OP_SUB: return "sub";
        case IR_OP_MUL: return "mul";
        case IR_OP_SDIV: return "sdiv";
        case IR_OP_SREM: return "srem";
        case IR_OP_FADD: return "fadd";
        case IR_OP_FSUB: return "fsub";
        case IR_OP_FMUL: return "fmul";
        case IR_OP_FDIV: return "fdiv";
        case IR_OP_SHL: return "shl";
        case IR_OP_LSHR: return "lshr";
        case IR_OP_ASHR: return "ashr";
        case IR_OP_AND: return "and";
        case IR_OP_OR: return "or";
        case IR_OP_XOR: return "xor";
        case IR_OP_ALLOCA: return "alloca";
        case IR_OP_LOAD: return "load";
        case IR_OP_STORE: return "store";
        case IR_OP_GETELEMENTPTR: return "getelementptr";
        case IR_OP_ICMP: return "icmp";
        case IR_OP_FCMP: return "fcmp";
        case IR_OP_PHI: return "phi";
        case IR_OP_CALL: return "call";
        case IR_OP_SITOFP: return "sitofp";
        case IR_OP_FPTOSI: return "fptosi";
        case IR_OP_ZEXT: return "zext";
        case IR_OP_FPEXT: return "fpext";
        case IR_OP_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

// --- 本地辅助函数：创建未定义值 ---
static IRValue* create_undef_value(MemoryPool* pool) {
    IRValue* val = (IRValue*)pool_alloc_z(pool, sizeof(IRValue));
    val->is_constant = false;
    val->type = NULL;
    val->name = NULL;
    return val;
}

// --- 主要的优化遍入口点 ---
bool run_mem2reg(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Mem2Reg: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running Mem2Reg on function @%s", func->name);
    }
    assert(func->module && func->module->pool);

    Mem2RegContext ctx = {0};
    ctx.func = func;
    ctx.pool = func->module->pool;
    ir_builder_init(&ctx.builder, func);

    // 1. 分析所有 alloca 指令，找到可提升的那些。
    analyze_allocas(&ctx);

    if (ctx.promotable_count == 0) {
        if (func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "No promotable allocas found in @%s.", func->name);
        }
        return false;
    }
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Found %d promotable allocas in @%s.", ctx.promotable_count, func->name);
    }

    // 2. 计算支配边界，并决定在哪些基本块中放置 PHI 节点。
    compute_phi_placement(&ctx);

    // 3. 在计算出的位置插入必要的 PHI 节点。
    insert_phi_nodes(&ctx);
    
    // 4. 重命名变量，将 load/store 替换为对 SSA 值的直接使用。
    rename_variables(&ctx);
    
    // 5. 清理，移除现在已经无用的 alloca 指令。
    for (int i = 0; i < ctx.promotable_count; ++i) {
        erase_instruction(ctx.promotables[i].alloca_instr);
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Mem2Reg finished for @%s", func->name);
    }
    return true; // 函数被修改
}

// --- 核心算法步骤的实现 ---

/**
 * @brief 检查一个 alloca 指令是否可以被提升到寄存器。
 * @details
 * 一个 alloca 可以被提升的条件是：
 * 1. 它分配的是标量类型（非数组、结构体）。
 * 2. 对其地址的所有使用都必须是 load 或 store 指令。
 * 3. 在 store 指令中，该地址必须作为被存储的目标，而不是被存储的值。
 */
static bool is_alloca_promotable(IRInstruction* alloca_instr) {
    Type* allocated_type = alloca_instr->dest->type->pointer.element_type;
    // 我们只提升简单的标量类型。聚合类型由 SROA 遍处理。
    if (allocated_type->kind == TYPE_ARRAY) {
        return false;
    }

    // 检查所有使用者
    for (IROperand* use = alloca_instr->dest->use_list_head; use; use = use->next_use) {
        IRInstruction* user = use->user;
        if (user->opcode == IR_OP_LOAD) {
            continue; // load 是合法使用
        }
        if (user->opcode == IR_OP_STORE && use == user->operand_head->next_in_instr) {
            continue; // 作为 store 的目标地址是合法使用
        }
        // 其他任何使用方式（如作为 GEP 的基址，或作为被存储的值）都使此 alloca 不可提升。
        // 注意：这里没有ASTContext，所以我们需要创建一个默认的LogConfig
        LogConfig default_log_config;
        logger_config_init_default(&default_log_config);
        LOG_DEBUG(&default_log_config, LOG_CATEGORY_IR_OPT, "Alloca %s not promotable: used by non-load/store instruction %s or as a stored value.",
                   alloca_instr->dest->name, get_opcode_name(user->opcode));
        return false;
    }
    return true;
}

/**
 * @brief 遍历函数入口块，找到所有可提升的 alloca，并收集它们的定义块信息。
 */
static void analyze_allocas(Mem2RegContext* ctx) {
    Worklist* promotable_list = create_worklist(ctx->pool, 16);
    
    // alloca 指令必须在函数入口块。
    for (IRInstruction* instr = ctx->func->entry->head; instr && instr->opcode == IR_OP_ALLOCA; instr = instr->next) {
        if (is_alloca_promotable(instr)) {
            worklist_add(promotable_list, instr);
        }
    }

    ctx->promotable_count = promotable_list->count;
    if (ctx->promotable_count == 0) return;
    
    ctx->promotables = (PromotableAlloca*)pool_alloc(ctx->pool, ctx->promotable_count * sizeof(PromotableAlloca));
    
    for (int i = 0; i < ctx->promotable_count; ++i) {
        PromotableAlloca* pa = &ctx->promotables[i];
        pa->alloca_instr = (IRInstruction*)promotable_list->items[i];
        pa->alloca_val = pa->alloca_instr->dest;
        pa->defining_blocks = bitset_create(ctx->func->block_count, ctx->pool);
        pa->phi_placement_blocks = bitset_create(ctx->func->block_count, ctx->pool);
        
        // 找到所有对该 alloca 进行 store 操作的块
        for (IROperand* use = pa->alloca_val->use_list_head; use; use = use->next_use) {
            if (use->user->opcode == IR_OP_STORE) {
                bitset_add(pa->defining_blocks, use->user->parent->post_order_id, ctx->func->module->log_config);
            }
        }
    }
}

/**
 * @brief 对每个可提升的 alloca，计算需要在哪些块中放置 PHI 节点。
 * @details
 * 使用迭代的支配边界算法：一个变量 `V` 的 PHI 节点需要被放置在 `V` 的定义块集合的
 * "迭代支配边界"（Iterated Dominance Frontier）中。
 */
static void compute_phi_placement(Mem2RegContext* ctx) {
    for (int i = 0; i < ctx->promotable_count; ++i) {
        PromotableAlloca* pa = &ctx->promotables[i];
        Worklist* worklist = create_worklist(ctx->pool, ctx->func->block_count);
        
        // 初始化工作列表，包含所有定义了该变量的块。
        for (int block_id = 0; block_id < ctx->func->block_count; ++block_id) {
            if (bitset_contains(pa->defining_blocks, block_id)) {
                worklist_add(worklist, ctx->func->reverse_post_order[block_id]);
            }
        }

        // 迭代计算支配边界，直到工作列表为空。
        while (worklist->count > 0) {
            IRBasicBlock* block = (IRBasicBlock*)worklist_pop(worklist);
            // 对当前块的支配边界中的每一个块 F
            for (int j = 0; j < block->dom_frontier_count; ++j) {
                IRBasicBlock* frontier_block = block->dom_frontier[j];
                int frontier_id = frontier_block->post_order_id;
                
                // 如果还没有为 F 放置 PHI 节点
                if (!bitset_contains(pa->phi_placement_blocks, frontier_id)) {
                    bitset_add(pa->phi_placement_blocks, frontier_id, ctx->func->module->log_config);
                    // 如果 F 本身不定义该变量，则将 F 加入工作列表，继续传播。
                    if (!bitset_contains(pa->defining_blocks, frontier_id)) {
                        worklist_add(worklist, frontier_block);
                    }
                }
            }
        }
    }
}

/**
 * @brief 根据 `phi_placement_blocks` 集合，在对应的基本块中插入 PHI 指令。
 */
static void insert_phi_nodes(Mem2RegContext* ctx) {
    for (int i = 0; i < ctx->promotable_count; ++i) {
        PromotableAlloca* pa = &ctx->promotables[i];
        
        for (int block_id = 0; block_id < ctx->func->block_count; ++block_id) {
            if (bitset_contains(pa->phi_placement_blocks, block_id)) {
                IRBasicBlock* block = ctx->func->reverse_post_order[block_id];
                // 设置 builder 的插入点到块的开头
                ir_builder_set_insertion_block_start(&ctx->builder, block);
                
                // 为 PHI 节点生成一个有意义的名字
                char name_buf[64];
                const char* base_name = pa->alloca_val->name ? pa->alloca_val->name : "tmp";
                if (base_name[0] == '%') base_name++;
                const char* suffix_pos = strstr(base_name, ".addr");
                if (suffix_pos) {
                    snprintf(name_buf, (suffix_pos - base_name) + 1, "%s", base_name);
                } else {
                    snprintf(name_buf, sizeof(name_buf), "%s", base_name);
                }
                
                Type* phi_type = pa->alloca_val->type->pointer.element_type;
                IRInstruction* phi = ir_builder_create_phi(&ctx->builder, phi_type, name_buf);
                // 关键：将 PHI 节点与它所代表的 alloca 关联起来，供后续重命名阶段使用。
                phi->phi_for_alloca = pa->alloca_instr;
            }
        }
    }
}

/**
 * @brief 变量重命名阶段的主驱动函数。
 * @details 初始化每个可提升变量的版本栈，然后从入口块开始递归地进行重命名。
 */
static void rename_variables(Mem2RegContext* ctx) {
    // 为每个可提升的 alloca 创建一个版本栈。
    VersionStackNode** stacks = (VersionStackNode**)pool_alloc(ctx->pool, ctx->promotable_count * sizeof(VersionStackNode*));
    
    // 创建一个全局的"未定义"值，用于初始化版本栈。
    ctx->undef_val = create_undef_value(ctx->pool);

    for (int i = 0; i < ctx->promotable_count; ++i) {
        stacks[i] = NULL;
        push_version(&stacks[i], ctx->undef_val, ctx->pool);
    }
    
    // 从支配树的根节点（即函数入口块）开始递归重命名。
    rename_recursive(ctx->func->entry, ctx, stacks);
}

/**
 * @brief 递归地遍历支配树，进行变量重命名。
 * @details 这是 SSA 构建算法的核心，基于 Cytron 等人的论文。
 */
static void rename_recursive(IRBasicBlock* block, Mem2RegContext* ctx, VersionStackNode** stacks) {
    // 记录在此块中对每个变量栈推送了多少个新版本
    int* pushed_counts = (int*)pool_alloc_z(ctx->pool, ctx->promotable_count * sizeof(int));

    // 1. 处理本块中的 PHI 节点：它们为对应的变量定义了新的版本。
    for (IRInstruction* instr = block->head; instr && instr->opcode == IR_OP_PHI; instr = instr->next) {
        for (int i = 0; i < ctx->promotable_count; ++i) {
            if (instr->phi_for_alloca == ctx->promotables[i].alloca_instr) {
                push_version(&stacks[i], instr->dest, ctx->pool);
                pushed_counts[i]++;
                break;
            }
        }
    }
    
    // 2. 遍历本块中的常规指令
    for (IRInstruction* instr = block->head; instr; instr = instr->next) {
        if (instr->opcode == IR_OP_LOAD) {
            // 如果是 load 一个可提升的 alloca
            for (int i = 0; i < ctx->promotable_count; ++i) {
                if (instr->operand_head->data.value == ctx->promotables[i].alloca_val) {
                    // 将所有对 load 结果的使用替换为当前版本栈顶的值。
                    replace_all_uses_with(NULL, instr->dest, top_version(stacks[i]));
                    // 标记此 load 指令为可删除。
                    mark_instruction_for_removal(instr);
                    break;
                }
            }
        } else if (instr->opcode == IR_OP_STORE) {
            // 如果是 store到一个可提升的 alloca
            for (int i = 0; i < ctx->promotable_count; ++i) {
                if (instr->operand_head->next_in_instr->data.value == ctx->promotables[i].alloca_val) {
                    // 将被 store 的值作为该变量的新版本，压入版本栈。
                    push_version(&stacks[i], instr->operand_head->data.value, ctx->pool);
                    pushed_counts[i]++; // 记录一次压栈
                    // 标记此 store 指令为可删除。
                    mark_instruction_for_removal(instr);
                    break;
                }
            }
        }
    }
    
    // 3. 填充所有后继块中 PHI 节点的操作数
    for (int i = 0; i < block->num_successors; ++i) {
        IRBasicBlock* succ = block->successors[i];
        for (IRInstruction* phi = succ->head; phi && phi->opcode == IR_OP_PHI; phi = phi->next) {
            // 找到与此 PHI 对应的 alloca
            for (int j = 0; j < ctx->promotable_count; ++j) {
                if (phi->phi_for_alloca == ctx->promotables[j].alloca_instr) {
                    // 将当前变量的最新版本作为来自本块(block)的入口值添加到PHI节点。
                    ir_phi_add_incoming(phi, top_version(stacks[j]), block);
                    break;
                }
            }
        }
    }

    // 4. 递归地访问支配树中的子节点
    for (int i = 0; i < block->dom_children_count; ++i) {
        rename_recursive(block->dom_children[i], ctx, stacks);
    }
    
    // 5. 回溯：清理本块中标记为可删除的指令，并弹出在此块中压入的版本。
    cleanup_removed_instructions(block);
    for (int i = 0; i < ctx->promotable_count; ++i) {
        for (int j = 0; j < pushed_counts[i]; ++j) {
            pop_version(&stacks[i]);
        }
    }
}


// --- 版本栈工具函数 ---

static void push_version(VersionStackNode** stack_top, IRValue* value, MemoryPool* pool) {
    VersionStackNode* new_node = (VersionStackNode*)pool_alloc(pool, sizeof(VersionStackNode));
    new_node->value = value;
    new_node->next = *stack_top;
    *stack_top = new_node;
}

static IRValue* top_version(VersionStackNode* stack_top) {
    assert(stack_top != NULL && "Attempted to get version from empty stack");
    return stack_top->value;
}

static void pop_version(VersionStackNode** stack_top) {
    assert(*stack_top != NULL && "Attempted to pop from empty stack");
    *stack_top = (*stack_top)->next;
}