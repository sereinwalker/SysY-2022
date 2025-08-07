/**
 * @file sroa.c
 * @brief 实现聚合的标量替换（Scalar Replacement of Aggregates, SROA）优化遍。
 * @details
 * 本文件实现了 SROA 算法，其核心目标是将对整个数组的栈分配（`alloca [10 x i32]`）
 * 拆分为对数组每个元素的单独栈分配（10 条 `alloca i32`）。
 * 流程如下：
 * 1.  **识别候选者**: 遍历函数入口块，找出所有为静态大小的数组分配空间的 `alloca` 指令。
 *     同时，必须保证对该 `alloca` 指针的所有使用都是 `getelementptr` (GEP) 指令，
 *     并且 GEP 的第一个索引是常量。
 * 2.  **分解**: 对每个候选的 `alloca`，为其每个元素创建一个新的、独立的 `alloca` 指令。
 * 3.  **重写使用者**: 遍历原始 `alloca` 的所有 GEP 使用者。根据 GEP 的常量索引，
 *     将其重写为直接使用新的、对应的元素 `alloca`。
 * 4.  **清理**: 移除原始的、大的 `alloca` 指令。
 * 5.  **迭代**: 将新创建的元素 `alloca`（如果它们本身也是数组类型）重新放入工作列表，
 *     以便递归地进行分解。
 */
#include "ir/transforms/sroa.h"
#include "ir/ir_utils.h"
#include "ir/ir_builder.h"
#include <string.h>
#include "ast.h"            // for Type::(anonymous union)::(anonymous), Typ...
#include "logger.h"         // for LOG_CATEGORY_IR_OPT, LOG_DEBUG
#include <stdio.h>
#include <ctype.h>


// --- 用于 SROA 分析的内部数据结构 ---

/**
 * @struct SROACandidate
 * @brief 存储一个可进行 SROA 的 `alloca` 的分析信息。
 */
typedef struct {
    IRInstruction* alloca_instr;    ///< 指向原始的 alloca 指令
    Type* array_type;               ///< 被分配的数组类型
    size_t num_elements;            ///< 数组第一维的元素数量
    Type* element_type;             ///< 数组的元素类型
    bool is_multi_dimensional;      ///< 元素本身是否仍然是数组
    int dimension_count;            ///< 数组的总维度数
} SROACandidate;

/**
 * @struct SROATransformation
 * @brief 在转换过程中存储映射关系（未使用）。
 */
typedef struct {
    IRValue* original_alloca;       ///< 原始的 alloca 地址
    IRValue** new_allocas;          ///< 指向新创建的元素 alloca 地址数组的指针
    size_t count;                   ///< 元素数量
    bool* processed;                ///< 标记是否已处理（未使用）
} SROATransformation;

// --- 辅助函数原型声明 ---
static bool is_alloca_sroa_able(IRInstruction* alloca_instr);
static void perform_sroa_on_alloca(IRInstruction* alloca_instr, Worklist* wl);
static void rewrite_gep_of_decomposed_array(IRInstruction* gep_instr, IRValue** new_element_allocas);
static SROACandidate analyze_sroa_candidate(IRInstruction* alloca_instr);
static bool can_decompose_array_type(Type* array_type);
static void create_element_allocas(SROACandidate* candidate, IRValue** new_allocas, IRBuilder* builder);
static void rewrite_all_gep_uses(IRInstruction* alloca_instr, IRValue** new_allocas, size_t num_element);
static char* generate_element_name(const char* base_name, size_t index, MemoryPool* pool);

// --- 主要入口点 ---
bool run_sroa(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SROA: No function or entry block");
        }
        return false;
    }
    
    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running SROA on function @%s", func->name);
    }
    
    bool changed = false;
    Worklist* wl = create_worklist(func->module->pool, 64);

    // 查找所有可进行 SROA 的数组 alloca
    for (IRInstruction* instr = func->entry->head; instr; instr = instr->next) {
        if (instr->opcode == IR_OP_ALLOCA && is_alloca_sroa_able(instr)) {
            worklist_add(wl, instr);
        }
    }

    if (wl->count > 0) {
        if (func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SROA: Found %d promotable allocas in @%s", wl->count, func->name);
        }
    }

    // 处理所有候选者
    while (wl->count > 0) {
        IRInstruction* alloca_instr = (IRInstruction*)worklist_pop(wl);
        if (alloca_instr->opcode == IR_OP_UNKNOWN) continue; // 已经被处理或删除

        perform_sroa_on_alloca(alloca_instr, wl);
        changed = true;
    }
    
    if (changed && func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SROA: Applied transformations in function @%s", func->name);
    }
    
    return changed;
}

// --- SROA 核心逻辑 ---

/**
 * @brief 检查一个 alloca 指令是否可以被 SROA 分解。
 * @details
 * 条件包括：
 * 1. 必须是为数组类型分配空间。
 * 2. 数组的大小必须是编译时已知的静态常量。
 * 3. 对该 alloca 地址的所有使用都必须是 GEP 指令。
 * 4. 所有这些 GEP 指令的第一个索引必须是常量，且不能越界。
 */
static bool is_alloca_sroa_able(IRInstruction* alloca_instr) {
    if (!alloca_instr || alloca_instr->opcode != IR_OP_ALLOCA) {
        return false;
    }
    IRValue* alloca_val = alloca_instr->dest;
    if (!alloca_val || !alloca_val->type || alloca_val->type->kind != TYPE_POINTER) {
        return false;
    }
    Type* allocated_type = alloca_val->type->pointer.element_type;
    if (!allocated_type || allocated_type->kind != TYPE_ARRAY) {
        return false;
    }
    if (!can_decompose_array_type(allocated_type)) {
        return false;
    }
    for (IROperand* use = alloca_val->use_list_head; use; use = use->next_use) {
        IRInstruction* user_instr = use->user;
        if (!user_instr || user_instr->opcode != IR_OP_GETELEMENTPTR) {
            return false;
        }
        if (user_instr->num_operands < 3) {
            return false;
        }
        IROperand* index0_op = user_instr->operand_head->next_in_instr;
        IROperand* index1_op = index0_op ? index0_op->next_in_instr : NULL;
        if (!index0_op || !index0_op->data.value || !index0_op->data.value->is_constant || index0_op->data.value->int_val != 0) {
            return false;
        }
        if (!index1_op || !index1_op->data.value || !index1_op->data.value->is_constant) {
            return false;
        }
        int idx = index1_op->data.value->int_val;
        size_t static_size = allocated_type->array.dimensions[0].static_size;
        if (idx < 0 || idx >= (int)static_size) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 递归检查数组类型的所有维度是否都是静态已知的。
 */
static bool can_decompose_array_type(Type* array_type) {
    if (!array_type || array_type->kind != TYPE_ARRAY) {
        return false;
    }
    
    // 检查数组是否有已知的静态大小
    if (array_type->array.dimensions[0].is_dynamic || 
        array_type->array.dimensions[0].static_size <= 0) {
        return false;
    }
    
    // 递归检查元素类型（如果元素也是数组）
    Type* element_type = array_type->array.element_type;
    if (element_type->kind == TYPE_ARRAY) {
        return can_decompose_array_type(element_type);
    }
    
    return true;
}

/**
 * @brief 分析一个 SROA 候选者并提取其关键信息。
 */
static SROACandidate analyze_sroa_candidate(IRInstruction* alloca_instr) {
    SROACandidate candidate = {0};
    candidate.alloca_instr = alloca_instr;
    
    Type* array_type = alloca_instr->dest->type->pointer.element_type;
    candidate.array_type = array_type;
    candidate.num_elements = array_type->array.dimensions[0].static_size;
    candidate.element_type = array_type->array.element_type;
    candidate.is_multi_dimensional = (array_type->array.dim_count > 1);
    candidate.dimension_count = array_type->array.dim_count;
    
    return candidate;
}

/**
 * @brief 对单个 `alloca` 指令执行 SROA 分解和重写。
 */
static void perform_sroa_on_alloca(IRInstruction* alloca_instr, Worklist* wl) {
    IRFunction* func = alloca_instr->parent->parent;
    SROACandidate candidate = analyze_sroa_candidate(alloca_instr);
    
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "SROA: Decomposing %s ([%zu x type]) into %zu pieces", 
              alloca_instr->dest->name, candidate.num_elements, candidate.num_elements);

    // 创建一个 IRBuilder 用于生成新的 alloca
    IRBuilder builder;
    ir_builder_init(&builder, func);

    // 为新创建的元素 alloca 分配存储空间
    IRValue** new_element_allocas = (IRValue**)pool_alloc(func->module->pool, 
                                                        candidate.num_elements * sizeof(IRValue*));
    
    // 为数组的每个元素创建新的 alloca 指令
    create_element_allocas(&candidate, new_element_allocas, &builder);

    // 重写所有对原始 alloca 的 GEP 使用
    rewrite_all_gep_uses(alloca_instr, new_element_allocas, candidate.num_elements);

    // 将新创建的 alloca（如果它们本身也是数组）添加到工作列表以进行递归分解
    for (size_t i = 0; i < candidate.num_elements; ++i) {
        if (new_element_allocas[i] && new_element_allocas[i]->def_instr) {
            if (is_alloca_sroa_able(new_element_allocas[i]->def_instr)) {
                worklist_add(wl, new_element_allocas[i]->def_instr);
            }
        }
    }

    // 移除原始的 alloca 指令
    erase_instruction(alloca_instr);
}

/**
 * @brief 为数组的每个元素创建新的 `alloca` 指令。
 */
static void create_element_allocas(SROACandidate* candidate, IRValue** new_allocas, IRBuilder* builder) {
    MemoryPool* pool = builder->module->pool;
    for (size_t i = 0; i < candidate->num_elements; ++i) {
        char* element_name = generate_element_name(candidate->alloca_instr->dest->name, i, pool);
        IRInstruction* piece_alloca = ir_builder_create_alloca(builder, candidate->element_type, element_name);
        new_allocas[i] = piece_alloca->dest;
    }
}

/**
 * @brief 为分解出的新元素生成一个有意义的名称。
 */
static char* generate_element_name(const char* base_name, size_t index, MemoryPool* pool) {
    char name_buf[128];
    if (!base_name) {
        snprintf(name_buf, sizeof(name_buf), "sroa.%zu", index);
    } else {
        const char* clean_name = base_name;
        if (clean_name[0] == '%') clean_name++;
        const char* suffix = strrchr(clean_name, '.');
        if (suffix && (strcmp(suffix, ".addr") == 0 || isdigit(suffix[1]))) {
            size_t len = suffix - clean_name;
            if (len >= sizeof(name_buf)) len = sizeof(name_buf) - 1;
            strncpy(name_buf, clean_name, len);
            name_buf[len] = '\0';
        } else {
            strncpy(name_buf, clean_name, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
        }
        size_t used = strlen(name_buf);
        snprintf(name_buf + used, sizeof(name_buf) - used, ".%zu", index);
    }
    return pool_strdup(pool, name_buf);
}

/**
 * @brief 重写所有使用原始 alloca 的 GEP 指令。
 */
static void rewrite_all_gep_uses(IRInstruction* alloca_instr, IRValue** new_allocas, size_t num_element) {
    (void)num_element;
    // 安全地遍历 use 链表（因为它在迭代过程中可能会被修改）
    IROperand* current_use = alloca_instr->dest->use_list_head;
    while (current_use) {
        IROperand* next_use = current_use->next_use; // 预先保存下一个 use
        IRInstruction* gep_instr = current_use->user;
        
        if (gep_instr && gep_instr->opcode == IR_OP_GETELEMENTPTR) {
            rewrite_gep_of_decomposed_array(gep_instr, new_allocas);
        }
        
        current_use = next_use;
    }
}

/**
 * @brief 重写单条 GEP 指令，使其指向新的元素 alloca。
 */
static void rewrite_gep_of_decomposed_array(IRInstruction* gep_instr, IRValue** new_element_allocas) {
    if (!gep_instr || gep_instr->opcode != IR_OP_GETELEMENTPTR) {
        return;
    }
    IROperand* index0_op = gep_instr->operand_head->next_in_instr;
    IROperand* index1_op = index0_op ? index0_op->next_in_instr : NULL;
    if (!index1_op || !index1_op->data.value || !index1_op->data.value->is_constant) {
        return;
    }
    int element_idx = index1_op->data.value->int_val;
    if (element_idx < 0) {
        return;
    }
    IRValue* new_base_alloca = new_element_allocas[element_idx];
    if (!new_base_alloca) {
        return;
    }
    if (gep_instr->num_operands == 3) {
        replace_all_uses_with(NULL, gep_instr->dest, new_base_alloca);
        erase_instruction(gep_instr);
    } else {
        int remaining_indices_count = gep_instr->num_operands - 3;
        if (remaining_indices_count <= 0) {
            replace_all_uses_with(NULL, gep_instr->dest, new_base_alloca);
            erase_instruction(gep_instr);
            return;
        }
        IRValue** new_indices = (IRValue**)pool_alloc(new_base_alloca->def_instr->parent->parent->module->pool, remaining_indices_count * sizeof(IRValue*));
        IROperand* current_op = index1_op->next_in_instr;
        for (int i = 0; i < remaining_indices_count; ++i) {
            new_indices[i] = current_op->data.value;
            current_op = current_op->next_in_instr;
        }
        IRBuilder builder;
        ir_builder_init(&builder, gep_instr->parent->parent);
        ir_builder_set_insertion_point(&builder, gep_instr);
        IRInstruction* new_gep = ir_builder_create_gep(&builder, new_base_alloca, new_indices, remaining_indices_count, "sroa.gep");
        replace_all_uses_with(NULL, gep_instr->dest, new_gep->dest);
        erase_instruction(gep_instr);
    }
}