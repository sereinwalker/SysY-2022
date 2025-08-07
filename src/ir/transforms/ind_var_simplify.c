/**
 * @file ind_var_simplify.c
 * @brief 实现一个基于数据流分析的归纳变量化简与强度削弱优化遍。
 * @details
 * 本文件实现了归纳变量（Induction Variable, IV）化简优化。其核心思想是，
 * 识别出循环中所有形式为 `j = i * A + B` 的变量，其中 `i` 是一个基本的循环
 * 计数器（基本归纳变量，BIV），而 `A` 和 `B` 是循环不变量。
 *
 * 然后，它通过"强度削弱"技术，为每个这样的派生归纳变量 `j` 创建一个新的、
 * 更简单的基本归纳变量 `j'`，其更新方式为 `j' = j' + (BIV.step * A)`。
 * 最终，所有对 `j` 的使用都被替换为对 `j'` 的使用，从而将循环内部的乘法运算
 * 移出到循环外，并代之以一个简单的加法。
 *
 * 算法流程：
 * 1.  **寻找基本归纳变量 (BIV)**：在循环头中找到 `i = phi [init, i_next]` 形式的变量。
 * 2.  **分析派生归纳变量 (DIV)**：通过不动点迭代，以拓扑序（RPO）遍历循环内所有指令，
 *     为每个值推导其 `i*A+B` 的形式。
 * 3.  **强度削弱**: 对分析出的、由乘法产生的复杂 DIV 进行变换，用新的、简单的
 *     加法型归纳变量替换它们。
 */
#include "ir/transforms/ind_var_simplify.h"
#include "ir/analysis/loop_analysis.h"
#include "ir/ir_builder.h"
#include "ir/ir_utils.h"
#include <string.h>
#include <stdint.h>                     // for uintptr_t
#include "ir/ir_data_structures.h"
#include "ast.h"                        // for pool_alloc
#include "logger.h"                     // for LOG_CATEGORY_IR_OPT, LOG_DEBUG


// --- 用于归纳变量分析的数据结构 ---

/** @brief 描述一个基本的归纳变量。 */
typedef struct BasicInductionVar { IRInstruction* phi; IRValue* initial_value; IRValue* step; IRBasicBlock* back_edge_block; } BasicInductionVar;
/** @brief 描述一个值与某个基本归纳变量之间的线性关系 (BIV * scale + offset)。 */
typedef struct IVInfo { const BasicInductionVar* biv; IRValue* scale; IRValue* offset; } IVInfo;
/** @brief 描述一个派生的归纳变量。 */
typedef struct DerivedInductionVar { IRValue* value; IVInfo info; } DerivedInductionVar;
/** @brief 用于 IVMap 的哈希表条目。 */
typedef struct IVMapEntry { IRValue* key; IVInfo value; struct IVMapEntry* next; } IVMapEntry;
/** @brief 用于存储从 IRValue* 到其 IVInfo 的映射的哈希表。 */
typedef struct IVMap { IVMapEntry** buckets; int num_buckets; MemoryPool* pool; } IVMap;

// 外部函数的前向声明
int is_basic_iv(Loop* loop, IRInstruction* instr, IVInfo* info);
Worklist* get_instructions_in_loop_topo_order(Loop* loop);
IRInstruction* ir_builder_create_binary_op(IRBuilder* builder, Opcode op, IRValue* lhs, IRValue* rhs, const char* name);
IRInstruction* ir_builder_create_mul(IRBuilder* builder, IRValue* lhs, IRValue* rhs, const char* name);
IRInstruction* ir_builder_create_add(IRBuilder* builder, IRValue* lhs, IRValue* rhs, const char* name);
void ir_builder_set_insertion_block_start(IRBuilder* builder, IRBasicBlock* block);
IRBasicBlock* get_loop_latch(Loop* loop);

// --- 上下文结构体定义 ---

/**
 * @brief 存储归纳变量化简遍执行期间所需状态的上下文。
 */
typedef struct IVSimplifyContext {
    Loop* loop;              ///< 当前正在处理的循环
    IRBuilder* builder;      ///< 用于创建新指令的构建器
    MemoryPool* pool;        ///< 用于内部分配的内存池
    IVMap iv_map;            ///< 存储 IV 分析结果的映射表
    BasicInductionVar* biv_list; ///< 循环中找到的所有基本归纳变量列表
    int biv_count;           ///< 基本归纳变量的数量
    bool changed;            ///< 标记 IR 是否被修改
} IVSimplifyContext;

// --- 本文件内静态函数的原型声明 ---
static bool simplify_loop_recursively(Loop* loop, IRBuilder* builder);
static void simplify_ivs_in_loop(IVSimplifyContext* ctx);
static void find_basic_ivs(IVSimplifyContext* ctx);
static void analyze_derived_ivs(IVSimplifyContext* ctx);
static void reduce_strength_of_divs(IVSimplifyContext* ctx);
static bool compute_iv_info_for_instr(IVSimplifyContext* ctx, IRInstruction* instr, IVInfo* result_info);
static IRValue* get_or_create_computation_in_preheader(IVSimplifyContext* ctx, Opcode op, IRValue* lhs, IRValue* rhs);
static bool is_loop_invariant(Loop* loop, IRValue* value);

// IVMap 函数
static void iv_map_init(IVMap* map, MemoryPool* pool);
static IVInfo* iv_map_get(IVMap* map, IRValue* key);
static void iv_map_put(IVMap* map, IRValue* key, IVInfo value);
static unsigned long iv_hash(IRValue* val) { return (uintptr_t)val >> 3; }


// --- 主入口函数 ---
bool run_ind_var_simplify(IRFunction* func) {
    if (!func || !func->top_level_loops) return false;
    bool changed_overall = false;
    IRBuilder builder;
    ir_builder_init(&builder, func);
    // 递归处理所有循环（从最内层开始）
    for (Loop* loop = func->top_level_loops; loop; loop = loop->next) {
        if (simplify_loop_recursively(loop, &builder)) {
            changed_overall = true;
        }
    }
    return changed_overall;
}

// --- 递归遍历循环 ---
static bool simplify_loop_recursively(Loop* loop, IRBuilder* builder) {
    // 只处理具有规范前置头（preheader）的简单循环
    if (!loop->preheader || loop->num_back_edges != 1) return false;

    IVSimplifyContext ctx = {0};
    ctx.loop = loop;
    ctx.builder = builder;
    ctx.pool = loop->header->parent->module->pool;
    ctx.changed = false;
    
    // 深度优先，先处理子循环
    for (int i = 0; i < loop->num_sub_loops; ++i) {
        if (simplify_loop_recursively(loop->sub_loops[i], builder)) {
            ctx.changed = true;
        }
    }
    
    simplify_ivs_in_loop(&ctx);
    
    return ctx.changed;
}

// --- 单个循环的核心处理逻辑 ---
static void simplify_ivs_in_loop(IVSimplifyContext* ctx) {
    // 1. 找到所有基本归纳变量 (BIV)
    find_basic_ivs(ctx);
    if (ctx->biv_count == 0) return;

    // 2. 分析所有指令，推导派生归纳变量 (DIV) 的线性形式
    analyze_derived_ivs(ctx);
    
    // 3. 对找到的 DIV 进行强度削弱变换
    reduce_strength_of_divs(ctx);
}

// --- IVMap 实现 ---
static void iv_map_init(IVMap* map, MemoryPool* pool) { 
    map->pool = pool; 
    map->num_buckets = 64; 
    map->buckets = (IVMapEntry**)pool_alloc_z(pool, map->num_buckets * sizeof(IVMapEntry*)); 
}
static IVInfo* iv_map_get(IVMap* map, IRValue* key) { 
    if (!key) return NULL; 
    unsigned long i = iv_hash(key) % map->num_buckets; 
    for (IVMapEntry* e = map->buckets[i]; e; e = e->next) {
        if (e->key == key) return &e->value; 
    }
    return NULL; 
}
static void iv_map_put(IVMap* map, IRValue* key, IVInfo value) { 
    unsigned long i = iv_hash(key) % map->num_buckets;
    for (IVMapEntry* e = map->buckets[i]; e; e = e->next) {
        if (e->key == key) {
            e->value = value;
            return;
        }
    }
    IVMapEntry* n = (IVMapEntry*)pool_alloc(map->pool, sizeof(IVMapEntry)); 
    n->key = key; n->value = value; n->next = map->buckets[i]; 
    map->buckets[i] = n;
}

// --- 辅助函数实现 ---

// 辅助函数：比较两个 IVInfo 是否相等
static bool iv_info_equals(const IVInfo* a, const IVInfo* b) {
    return a->biv == b->biv && a->scale == b->scale && a->offset == b->offset;
}

// 辅助函数：获取表示"不可分析"的 Bottom IVInfo
static const IVInfo* get_bottom_iv_info() {
    static IVInfo bottom = {NULL, NULL, NULL};
    return &bottom;
}

// 在循环中查找所有基本归纳变量。
static void find_basic_ivs(IVSimplifyContext* ctx) {
    Loop* loop = ctx->loop;
    int max_bivs = 16;
    ctx->biv_list = (BasicInductionVar*)pool_alloc(ctx->pool, max_bivs * sizeof(BasicInductionVar));
    ctx->biv_count = 0;

    for (IRInstruction* instr = loop->header->head; instr && instr->opcode == IR_OP_PHI; instr = instr->next) {
        if (is_basic_iv(loop, instr, (IVInfo*)&ctx->biv_list[ctx->biv_count])) {
            ctx->biv_count++;
            if (ctx->biv_count >= max_bivs) break;
        }
    }
}

// 通过不动点迭代，分析循环内所有值，推导它们的 IVInfo。
static void analyze_derived_ivs(IVSimplifyContext* ctx) {
    iv_map_init(&ctx->iv_map, ctx->pool);
    Worklist* topo_list = get_instructions_in_loop_topo_order(ctx->loop);

    IRValue* zero = ir_builder_create_const_int(ctx->builder, 0);
    IRValue* one = ir_builder_create_const_int(ctx->builder, 1);

    // 1. 初始化映射表：循环不变量的形式是 i*0 + C，基本归纳变量是 i*1 + 0。
    for (int i = 0; i < topo_list->count; ++i) {
        IRInstruction* instr = (IRInstruction*)topo_list->items[i];
        for (IROperand* op = instr->operand_head; op; op = op->next_in_instr) {
            if (is_loop_invariant(ctx->loop, op->data.value)) {
                iv_map_put(&ctx->iv_map, op->data.value, (IVInfo){.biv = NULL, .scale = zero, .offset = op->data.value});
            }
        }
    }
    for (int i = 0; i < ctx->biv_count; ++i) {
        iv_map_put(&ctx->iv_map, ctx->biv_list[i].phi->dest, (IVInfo){.biv = &ctx->biv_list[i], .scale = one, .offset = zero});
    }

    // 2. 不动点迭代：反复计算 -> 比较 -> 更新，直到没有信息再变化。
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < topo_list->count; ++i) {
            IRInstruction* instr = (IRInstruction*)topo_list->items[i];
            if (!instr->dest) continue;

            IVInfo new_info;
            if (!compute_iv_info_for_instr(ctx, instr, &new_info)) continue;
            IVInfo* old_info = iv_map_get(&ctx->iv_map, instr->dest);
            if (!old_info || !iv_info_equals(old_info, &new_info)) {
                iv_map_put(&ctx->iv_map, instr->dest, new_info);
                changed = true;
            }
        }
    }
}

// 对单条指令进行符号推导，计算其结果的 IVInfo。
static bool compute_iv_info_for_instr(IVSimplifyContext* ctx, IRInstruction* instr, IVInfo* result_info) {
    if (instr->opcode == IR_OP_PHI) return false;
    if (instr->num_operands != 2) return false;
    
    IROperand* op1 = instr->operand_head;
    IROperand* op2 = op1->next_in_instr;

    IVInfo* info1 = iv_map_get(&ctx->iv_map, op1->data.value);
    IVInfo* info2 = iv_map_get(&ctx->iv_map, op2->data.value);

    if (!info1 || !info2) return false;

    // --- 核心推导逻辑 ---

    // 情况 1: 两个操作数都基于同一个 BIV: j = (i*A + B) + (i*C + D)  => j = i*(A+C) + (B+D)
    if (info1->biv && info1->biv == info2->biv) {
        result_info->biv = info1->biv;
        switch (instr->opcode) {
            case IR_OP_ADD:
                result_info->scale  = get_or_create_computation_in_preheader(ctx, IR_OP_ADD, info1->scale, info2->scale);
                result_info->offset = get_or_create_computation_in_preheader(ctx, IR_OP_ADD, info1->offset, info2->offset);
                return true;
            case IR_OP_SUB:
                result_info->scale  = get_or_create_computation_in_preheader(ctx, IR_OP_SUB, info1->scale, info2->scale);
                result_info->offset = get_or_create_computation_in_preheader(ctx, IR_OP_SUB, info1->offset, info2->offset);
                return true;
            default: *result_info = *get_bottom_iv_info(); return true;
        }
    }

    // 情况 2: 一个是归纳变量，另一个是循环不变量: j = (i*A + B) op C
    IVInfo* iv_info = NULL;
    IRValue* invariant_val = NULL;

    if (info1->biv && !info2->biv) { // op1 是 IV, op2 是不变量
        iv_info = info1; invariant_val = op2->data.value;
    } else if (!info1->biv && info2->biv) { // op2 是 IV, op1 是不变量
        iv_info = info2; invariant_val = op1->data.value;
        if (instr->opcode == IR_OP_SUB) {
            // 特殊处理 c - (i*A + B) = i*(-A) + (c-B)
            result_info->biv = iv_info->biv;
            IRValue* zero = ir_builder_create_const_int(ctx->builder, 0);
            result_info->scale = get_or_create_computation_in_preheader(ctx, IR_OP_SUB, zero, iv_info->scale);
            result_info->offset = get_or_create_computation_in_preheader(ctx, IR_OP_SUB, invariant_val, iv_info->offset);
            return true;
        }
    } else { // 两个都是不变量，或基于不同的 BIV
        *result_info = *get_bottom_iv_info();
        return true;
    }

    result_info->biv = iv_info->biv;
    switch (instr->opcode) {
        case IR_OP_ADD: result_info->scale = iv_info->scale; result_info->offset = get_or_create_computation_in_preheader(ctx, IR_OP_ADD, iv_info->offset, invariant_val); return true;
        case IR_OP_SUB: result_info->scale = iv_info->scale; result_info->offset = get_or_create_computation_in_preheader(ctx, IR_OP_SUB, iv_info->offset, invariant_val); return true;
        case IR_OP_MUL: result_info->scale = get_or_create_computation_in_preheader(ctx, IR_OP_MUL, iv_info->scale, invariant_val); result_info->offset = get_or_create_computation_in_preheader(ctx, IR_OP_MUL, iv_info->offset, invariant_val); return true;
        default: *result_info = *get_bottom_iv_info(); return true;
    }
}

// 在 preheader 中获取或创建一个循环不变量计算。
static IRValue* get_or_create_computation_in_preheader(IVSimplifyContext* ctx, Opcode op, IRValue* lhs, IRValue* rhs) {
    IRBasicBlock* preheader = ctx->loop->preheader;
    // 检查是否已存在相同的计算（公共子表达式消除的简化版）
    for (IRInstruction* instr = preheader->head; instr != preheader->tail; instr = instr->next) {
        if (instr->opcode == op && instr->num_operands == 2) {
            if (instr->operand_head->data.value == lhs && instr->operand_head->next_in_instr->data.value == rhs) return instr->dest;
            if ((op == IR_OP_ADD || op == IR_OP_MUL) && instr->operand_head->data.value == rhs && instr->operand_head->next_in_instr->data.value == lhs) return instr->dest;
        }
    }
    // 如果不存在，则在 preheader 的末尾（终结符前）创建一个新的
    ir_builder_set_insertion_point(ctx->builder, preheader->tail);
    return ir_builder_create_binary_op(ctx->builder, op, lhs, rhs, "iv.calc")->dest;
}

// 对所有可简化的派生归纳变量执行强度削弱。
static void reduce_strength_of_divs(IVSimplifyContext* ctx) {
    Worklist* reducible_divs = create_worklist(ctx->pool, 16);
    
    // 收集所有可以进行强度削弱的 DIV
    for (int i = 0; i < ctx->iv_map.num_buckets; ++i) {
        for (IVMapEntry* e = ctx->iv_map.buckets[i]; e; e = e->next) {
            if (e->value.biv != NULL) {
                // 启发式规则：只对由乘法产生的复杂DIV进行削弱，因为这是收益最大的地方
                if (e->key->def_instr && e->key->def_instr->opcode == IR_OP_MUL) {
                    worklist_add(reducible_divs, e);
                }
            }
        }
    }

    if (reducible_divs->count > 0) {
        if (ctx->loop->header->parent->module && ctx->loop->header->parent->module->log_config) {
            LOG_DEBUG(ctx->loop->header->parent->module->log_config, LOG_CATEGORY_IR_OPT, "IV_SIMPLIFY: Found %d DIVs to reduce in loop with header %s.", reducible_divs->count, ctx->loop->header->label);
        }
        ctx->changed = true;

        while (reducible_divs->count > 0) {
            IVMapEntry* div_entry = (IVMapEntry*)worklist_pop(reducible_divs);
            const IVInfo* info = &div_entry->value;
            IRValue* div_val = div_entry->key;
            
            // 1. 在 preheader 中创建新归纳变量的初始值: new_init = BIV.init * scale + offset
            ir_builder_set_insertion_point(ctx->builder, ctx->loop->preheader->tail);
            IRValue* init_mul = ir_builder_create_mul(ctx->builder, info->biv->initial_value, info->scale, "new_iv.init.mul")->dest;
            IRValue* new_init = ir_builder_create_add(ctx->builder, init_mul, info->offset, "new_iv.init")->dest;

            // 2. 在 preheader 中创建新归纳变量的步进值: new_step = BIV.step * scale
            IRValue* new_step = ir_builder_create_mul(ctx->builder, info->biv->step, info->scale, "new_iv.step")->dest;

            // 3. 在循环头中创建新的 PHI 节点
            ir_builder_set_insertion_block_start(ctx->builder, ctx->loop->header);
            IRInstruction* new_phi = ir_builder_create_phi(ctx->builder, div_val->type, "new_iv");
            
            // 4. 在循环 latch 块中创建新的更新指令
            IRBasicBlock* latch = get_loop_latch(ctx->loop);
            ir_builder_set_insertion_point(ctx->builder, latch->tail);
            IRValue* new_update = ir_builder_create_add(ctx->builder, new_phi->dest, new_step, "new_iv.update")->dest;
            
            // 5. 连接 PHI 节点的入口
            ir_phi_add_incoming(new_phi, new_init, ctx->loop->preheader);
            ir_phi_add_incoming(new_phi, new_update, latch);

            // 6. 将所有对旧的、复杂的 DIV 的使用，替换为对新的、简单的 PHI 节点的使用
            replace_all_uses_with(NULL, div_val, new_phi->dest);
        }
    }
}

// 检查一个值是否是循环不变量。
static bool is_loop_invariant(Loop* loop, IRValue* value) { 
    if (value->is_constant) return true; 
    // 如果值的定义指令不在循环内部，则该值为不变量
    if (value->def_instr) {
        return !bitset_contains(loop->loop_blocks_bs, value->def_instr->parent->post_order_id);
    }
    return true; // 函数参数也被视为相对于循环的不变量
}

// 检查一个PHI指令是否表示基本归纳变量
int is_basic_iv(Loop* loop, IRInstruction* instr, IVInfo* info) {
    if (!instr || instr->opcode != IR_OP_PHI || !instr->dest) return 0;
    if (instr->num_operands != 4) return 0; // PHI指令有4个操作数：值1, 块1, 值2, 块2
    
    IROperand* op1 = instr->operand_head;
    IROperand* op2 = op1->next_in_instr;
    IROperand* op3 = op2->next_in_instr;
    IROperand* op4 = op3->next_in_instr;
    
    // 检查操作数类型：值-基本块-值-基本块的模式
    if (op1->kind != IR_OP_KIND_VALUE || op2->kind != IR_OP_KIND_BASIC_BLOCK ||
        op3->kind != IR_OP_KIND_VALUE || op4->kind != IR_OP_KIND_BASIC_BLOCK) {
        return 0;
    }
    
    IRValue* val1 = op1->data.value;
    IRBasicBlock* bb1 = op2->data.bb;
    IRValue* val2 = op3->data.value;
    IRBasicBlock* bb2 = op4->data.bb;
    
    // 一个操作数应该来自preheader，另一个来自latch
    IRBasicBlock* preheader = loop->preheader;
    IRBasicBlock* latch = get_loop_latch(loop);
    
    if (!preheader || !latch) return 0;
    
    IRValue* init_val = NULL;
    IRValue* step_val = NULL;
    IRBasicBlock* back_edge_block = NULL;
    
    // 确定哪个是初始值，哪个是步进值
    if (bb1 == preheader && bb2 == latch) {
        init_val = val1;
        step_val = val2;
        back_edge_block = latch;
    } else if (bb1 == latch && bb2 == preheader) {
        init_val = val2;
        step_val = val1;
        back_edge_block = latch;
    } else {
        return 0; // 不是标准的归纳变量模式
    }
    
    // 检查步进值是否是循环不变量
    if (!is_loop_invariant(loop, step_val)) return 0;
    
    // 检查初始值是否是循环不变量
    if (!is_loop_invariant(loop, init_val)) return 0;
    
    // 填充IVInfo
    if (info) {
        // 使用内存池分配BasicInductionVar
        BasicInductionVar* biv = (BasicInductionVar*)pool_alloc(loop->header->parent->module->pool, sizeof(BasicInductionVar));
        biv->phi = instr;
        biv->initial_value = init_val;
        biv->step = step_val;
        biv->back_edge_block = back_edge_block;
        
        info->biv = biv;
        // 创建常量值，使用正确的builder
        IRBuilder temp_builder;
        ir_builder_init(&temp_builder, loop->header->parent);
        info->scale = ir_builder_create_const_int(&temp_builder, 1); // 基本归纳变量的scale是1
        info->offset = ir_builder_create_const_int(&temp_builder, 0); // 基本归纳变量的offset是0
    }
    
    return 1;
}

// 获取循环的latch块（回边源块）
IRBasicBlock* get_loop_latch(Loop* loop) {
    if (!loop || loop->num_back_edges != 1) return NULL;
    
    // 对于简单循环，只有一个回边
    return loop->back_edges[0];
}