/**
 * @file ast.c
 * @brief 抽象语法树（AST）实现文件
 * @details
 * 本文件实现了 `ast.h` 中声明的函数，负责AST的创建、管理和销毁。
 * 主要功能包括：
 * 1.  **内存池管理**：实现了一个高效的区域内存分配器（Arena Allocator），
 *     用于快速分配AST节点、类型等对象，并在编译结束后一次性释放所有内存。
 * 2.  **类型创建**：实现了创建基础类型、数组、函数等类型对象的工厂函数。
 * 3.  **AST节点创建**：实现了为各种语法结构创建AST节点的工厂函数。
 *     所有节点都通过内存池进行分配，并自动设置父节点指针。
 * 4.  **调试支持**：提供了打印AST结构和类型信息的函数，便于开发和调试。
 */

#include "ast.h"
#include "symbol_table.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// --- 静态函数前向声明 (Static Function Prototypes) ---

/** @brief 创建一个通用的AST节点，并进行基本初始化。*/
static ASTNode* create_node(ASTContext* ctx, ASTNodeType type, SourceLocation loc);
/** @brief 设置子节点的父节点指针。*/
static void set_parent(ASTNode* parent, ASTNode* child);
/** @brief 批量为列表中的所有子节点设置父节点指针。*/
static void set_parent_for_list(ASTNode* parent, ASTNode** list, size_t count);
/** @brief 将操作符类型枚举转换为可读的字符串，用于调试打印。*/
const char* operator_type_to_string(OperatorType op);

#define BLOCK_SIZE 4096  ///< 内存池每次分配新内存块的默认大小

// ================================
// 1. 内存池实现 (Memory Pool Implementation)
// ================================

/**
 * @struct Block
 * @brief 内存池中的一个内存块。
 */
typedef struct Block {
    void* memory;           ///< 指向实际内存区域的指针
    size_t size;            ///< 内存块的总大小
    size_t used;            ///< 已使用的字节数
    struct Block* next;     ///< 指向下一个内存块的指针，形成链表
} Block;

/**
 * @struct MemoryPool
 * @brief 内存池管理器。
 * @details 管理一个Block链表，所有内存分配都在当前Block上进行。
 *          当当前Block空间不足时，会分配一个新的Block。
 */
struct MemoryPool {
    Block* first;           ///< 指向第一个内存块
    Block* current;         ///< 指向当前正在进行分配的内存块
};

/**
 * @brief 创建并初始化一个空的内存池。
 * @return 返回指向新创建的 MemoryPool 的指针。
 * @note 如果内存分配失败，程序将中止。
 */
MemoryPool* create_memory_pool() {
    MemoryPool* pool = malloc(sizeof(MemoryPool));
    if (pool == NULL) {
        perror("FATAL: Failed to allocate memory for MemoryPool");
        exit(EXIT_FAILURE);
    }
    pool->first = NULL;
    pool->current = NULL;
    return pool;
}

/**
 * @brief 从内存池中分配指定大小的内存。
 * @details 这是一个高效的分配方式，仅通过移动指针来完成。如果当前内存块空间不足，
 *          会自动分配一个新的内存块。分配的内存会对齐到8字节边界。
 * @param pool 内存池指针。
 * @param size 需要分配的字节数。
 * @return 返回一个指向已分配内存的 void 指针。
 * @note 如果内存分配失败，程序将中止。
 */
void* pool_alloc(MemoryPool* pool, size_t size) {
    if (pool == NULL) {
        fprintf(stderr, "FATAL: pool_alloc called with a NULL MemoryPool.\n");
        exit(EXIT_FAILURE);
    }
    // 对齐到8字节，以提高性能并避免某些平台的对齐问题
    size = (size + 7) & ~7;

    // 如果没有当前块，或者当前块剩余空间不足，则分配新块
    if (!pool->current || (pool->current->used + size) > pool->current->size) {
        // 如果请求的大小大于默认块大小，则分配一个更大的块
        size_t block_size = (size > BLOCK_SIZE) ? size * 2 : BLOCK_SIZE;
        Block* new_block = malloc(sizeof(Block));
        if (new_block == NULL) {
            perror("FATAL: Failed to allocate memory for Block");
            exit(EXIT_FAILURE);
        }
        new_block->memory = malloc(block_size);
        if (new_block->memory == NULL) {
            perror("FATAL: Failed to allocate memory for Block's internal buffer");
            free(new_block);
            exit(EXIT_FAILURE);
        }
        new_block->size = block_size;
        new_block->used = 0;
        new_block->next = NULL;

        // 将新块链接到链表
        if (!pool->first) {
            pool->first = new_block;
        } else {
            pool->current->next = new_block;
        }
        pool->current = new_block;
    }

    // 从当前块中分配内存
    void* ptr = (char*)pool->current->memory + pool->current->used;
    pool->current->used += size;
    return ptr;
}

/**
 * @brief 销毁内存池并释放其管理的所有内存块。
 * @param pool 要销毁的内存池指针。
 */
void destroy_memory_pool(MemoryPool* pool) {
    if (!pool) return;
    Block* block = pool->first;
    while (block) {
        Block* next = block->next;
        free(block->memory);
        free(block);
        block = next;
    }
    free(pool);
}

/**
 * @brief 在内存池中分配空间并复制一个字符串。
 * @param pool 内存池指针。
 * @param s 要复制的源字符串。
 * @return 返回一个指向内存池中新字符串的指针。如果源字符串为NULL，返回NULL。
 */
char* pool_strdup(MemoryPool* pool, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* new_s = pool_alloc(pool, len);
    memcpy(new_s, s, len);
    return new_s;
}

// ================================
// 2. 类型系统实现 (Type System Implementation)
// ================================

Type* create_basic_type(BasicType basic, bool is_const, MemoryPool* pool) {
    Type* type = pool_alloc(pool, sizeof(Type));
    type->kind = TYPE_BASIC;
    type->is_const = is_const;
    type->basic = basic;
    return type;
}

Type* create_array_type(Type* element_type, ArrayDimension* dims, size_t dim_count, bool is_const, MemoryPool* pool) {
    Type* type = pool_alloc(pool, sizeof(Type));
    type->kind = TYPE_ARRAY;
    type->is_const = is_const;
    type->array.element_type = element_type;
    type->array.dimensions = dims; // 假设 dims 已经从内存池分配
    type->array.dim_count = dim_count;
    return type;
}

Type* create_pointer_type(Type* element_type, bool is_const, MemoryPool* pool) {
    Type* type = pool_alloc(pool, sizeof(Type));
    type->kind = TYPE_POINTER;
    type->is_const = is_const;
    type->pointer.element_type = element_type;
    return type;
}

Type* create_function_type(Type* return_type, Type** param_types, size_t param_count, bool is_variadic, MemoryPool* pool) {
    Type* type = pool_alloc(pool, sizeof(Type));
    type->kind = TYPE_FUNCTION;
    type->is_const = false; // 函数类型本身不是const
    type->function.return_type = return_type;
    type->function.param_count = param_count;
    type->function.is_variadic = is_variadic;
    if (param_count > 0 && param_types != NULL) {
        // 复制参数类型列表到内存池
        size_t size = param_count * sizeof(Type*);
        Type** new_params = pool_alloc(pool, size);
        memcpy(new_params, param_types, size);
        type->function.param_types = new_params;
    } else {
        type->function.param_types = NULL;
    }
    return type;
}

Type* create_function_type_from_params(Type* return_type, ASTNode** param_nodes, size_t param_count, bool is_variadic, MemoryPool* pool) {
    Type* type = pool_alloc(pool, sizeof(Type));
    type->kind = TYPE_FUNCTION;
    type->is_const = false; // 函数类型本身不是const
    type->function.return_type = return_type;
    type->function.param_count = param_count;
    type->function.is_variadic = is_variadic;
    if (param_count > 0 && param_nodes != NULL) {
        // 直接从参数节点中提取类型，避免中间数组分配
        size_t size = param_count * sizeof(Type*);
        Type** param_types = pool_alloc(pool, size);
        for (size_t i = 0; i < param_count; ++i) {
            param_types[i] = param_nodes[i]->func_param.param_type;
        }
        type->function.param_types = param_types;
    } else {
        type->function.param_types = NULL;
    }
    return type;
}

Type* create_void_type(MemoryPool* pool) {
    Type* type = pool_alloc(pool, sizeof(Type));
    type->kind = TYPE_VOID;
    type->is_const = false;
    return type;
}

// ================================
// 3. AST上下文管理 (AST Context Management)
// ================================

ASTContext* create_ast_context() {
    ASTContext* ctx = (ASTContext*)calloc(1, sizeof(ASTContext));
    ctx->pool = create_memory_pool();
    // 直接初始化 ErrorContext
    init_error_context(&ctx->errors, 32);
    ctx->global_scope = create_symbol_table(ctx, NULL); // 创建全局符号表
    // 初始化日志配置为默认值
    logger_config_init_default(&ctx->log_config);
    return ctx;
}

void destroy_ast_context(ASTContext* ctx) {
    if (!ctx) return;
    // 释放 ErrorContext 的内部数组
    free_error_context(&ctx->errors);
    // 销毁内存池（会释放符号表、AST节点等）
    destroy_memory_pool(ctx->pool);
    // 最后释放 ASTContext 本身
    free(ctx);
}

// ================================
// 4. AST节点工厂函数 (AST Node Factory Functions)
// ================================

/**
 * @brief 创建一个通用的AST节点。
 * @details 这是所有节点创建函数的内部辅助函数。它从内存池分配一个ASTNode，
 *          并设置其类型、位置信息和默认值。
 * @param ctx AST上下文。
 * @param type 要创建的节点类型。
 * @param loc 节点在源文件中的位置。
 * @return 指向新创建的ASTNode的指针。
 */
static ASTNode* create_node(ASTContext* ctx, ASTNodeType type, SourceLocation loc) {
    ASTNode* node = (ASTNode*)pool_alloc(ctx->pool, sizeof(ASTNode));
    memset(node, 0, sizeof(ASTNode));
    node->node_type = type;
    node->loc = loc;
    node->parent = NULL; // 默认父指针为NULL
    return node;
}

ASTNode* create_var_decl(ASTContext* ctx, const char* name, const Type* type, const ASTNode* init, SourceLocation loc) {
    assert(ctx && "Context must not be null");
    assert(name && strlen(name) > 0 && "Name must not be null or empty");
    ASTNode* node = create_node(ctx, AST_VAR_DECL, loc);
    node->var_decl.name = pool_strdup(ctx->pool, name);
    node->var_decl.var_type = (Type*)type;
    node->var_decl.init_value = (ASTNode*)init;
    set_parent(node, (ASTNode*)init);
    return node;
}

ASTNode* create_const_decl(ASTContext* ctx, const char* name, const Type* type, const ASTNode* value, SourceLocation loc) {
    assert(ctx && "Context must not be null");
    assert(name && strlen(name) > 0 && "Name must not be null or empty");
    ASTNode* node = create_node(ctx, AST_CONST_DECL, loc);
    node->const_decl.name = pool_strdup(ctx->pool, name);
    node->const_decl.const_type = (Type*)type;
    node->const_decl.value = (ASTNode*)value;
    set_parent(node, (ASTNode*)value);
    return node;
}

ASTNode* create_func_param(ASTContext* ctx, const char* name, const Type* type, SourceLocation loc) {
    assert(ctx && name);
    ASTNode* node = create_node(ctx, AST_FUNC_PARAM, loc);
    node->func_param.name = pool_strdup(ctx->pool, name);
    node->func_param.param_type = (Type*)type;
    return node;
}

ASTNode* create_func_decl(ASTContext* ctx, const char* name, const Type* return_type, ASTNode** params, size_t param_count, ASTNode* body, SourceLocation loc) {
    assert(ctx && name);
    if (strcmp(name, "main") == 0) {
        ctx->has_main = true;
    }
    ASTNode* node = create_node(ctx, AST_FUNC_DECL, loc);
    node->func_decl.func_name = pool_strdup(ctx->pool, name);
    node->func_decl.return_type = (Type*)return_type;
    node->func_decl.params = params;
    node->func_decl.param_count = param_count;
    node->func_decl.body = body;
    set_parent_for_list(node, params, param_count);
    set_parent(node, body);
    return node;
}

ASTNode* create_compound_stmt(ASTContext* ctx, ASTNode** items, size_t count, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_COMPOUND_STMT, loc);
    node->compound_stmt.items = items;
    node->compound_stmt.item_count = count;
    node->compound_stmt.scope = NULL;
    set_parent_for_list(node, items, count);
    return node;
}

ASTNode* create_if_stmt(ASTContext* ctx, const ASTNode* cond, const ASTNode* then_stmt, const ASTNode* else_stmt, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_IF_STMT, loc);
    node->if_stmt.cond = (ASTNode*)cond;
    node->if_stmt.then_stmt = (ASTNode*)then_stmt;
    node->if_stmt.else_stmt = (ASTNode*)else_stmt;
    set_parent(node, (ASTNode*)cond);
    set_parent(node, (ASTNode*)then_stmt);
    set_parent(node, (ASTNode*)else_stmt);
    return node;
}

ASTNode* create_while_stmt(ASTContext* ctx, const ASTNode* cond, const ASTNode* body, SourceLocation loc) {
    assert(ctx && cond && body);
    ASTNode* node = create_node(ctx, AST_WHILE_STMT, loc);
    node->while_stmt.cond = (ASTNode*)cond;
    node->while_stmt.body = (ASTNode*)body;
    set_parent(node, (ASTNode*)cond);
    set_parent(node, (ASTNode*)body);
    return node;
}

ASTNode* create_return_stmt(ASTContext* ctx, const ASTNode* value, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_RETURN_STMT, loc);
    node->return_stmt.value = (ASTNode*)value;
    set_parent(node, (ASTNode*)value);
    return node;
}

ASTNode* create_break_stmt(ASTContext* ctx, SourceLocation loc) {
    assert(ctx);
    return create_node(ctx, AST_BREAK_STMT, loc);
}

ASTNode* create_continue_stmt(ASTContext* ctx, SourceLocation loc) {
    assert(ctx);
    return create_node(ctx, AST_CONTINUE_STMT, loc);
}

ASTNode* create_expr_stmt(ASTContext* ctx, const ASTNode* expr, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_EXPR_STMT, loc);
    node->expr_stmt.expr = (ASTNode*)expr;
    set_parent(node, (ASTNode*)expr);
    return node;
}

ASTNode* create_assign_stmt(ASTContext* ctx, const ASTNode* lval, const ASTNode* expr, SourceLocation loc) {
    assert(ctx && lval && expr);
    ASTNode* node = create_node(ctx, AST_ASSIGN_STMT, loc);
    node->assign_stmt.lval = (ASTNode*)lval;
    node->assign_stmt.expr = (ASTNode*)expr;
    set_parent(node, (ASTNode*)lval);
    set_parent(node, (ASTNode*)expr);
    return node;
}

ASTNode* create_binary_expr(ASTContext* ctx, OperatorType op, const ASTNode* left, const ASTNode* right, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_BINARY_EXPR, loc);
    node->binary_expr.op = op;
    node->binary_expr.left = (ASTNode*)left;
    node->binary_expr.right = (ASTNode*)right;
    set_parent(node, (ASTNode*)left);
    set_parent(node, (ASTNode*)right);
    return node;
}

ASTNode* create_unary_expr(ASTContext* ctx, OperatorType op, const ASTNode* operand, SourceLocation loc) {
    assert(ctx && operand);
    assert(op >= OP_ADD && op <= OP_NOT);
    ASTNode* node = create_node(ctx, AST_UNARY_EXPR, loc);
    node->unary_expr.op = op;
    node->unary_expr.operand = (ASTNode*)operand;
    set_parent(node, (ASTNode*)operand);
    return node;
}

ASTNode* create_call_expr(ASTContext* ctx, ASTNode* callee_expr, ASTNode** args, size_t arg_count, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_CALL_EXPR, loc);
    node->call_expr.callee_expr = callee_expr;
    node->call_expr.args = args;
    node->call_expr.arg_count = arg_count;
    set_parent(node, callee_expr);
    set_parent_for_list(node, args, arg_count);
    return node;
}

ASTNode* create_array_access(ASTContext* ctx, const ASTNode* array, const ASTNode* index, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_ARRAY_ACCESS, loc);
    node->array_access.array = (ASTNode*)array;
    node->array_access.index = (ASTNode*)index;
    set_parent(node, (ASTNode*)array);
    set_parent(node, (ASTNode*)index);
    return node;
}

ASTNode* create_identifier(ASTContext* ctx, const char* name, SourceLocation loc) {
    assert(ctx && name);
    ASTNode* node = create_node(ctx, AST_IDENTIFIER, loc);
    node->identifier.name = pool_strdup(ctx->pool, name);
    return node;
}

ASTNode* create_int_constant(ASTContext* ctx, int value, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_CONSTANT, loc);
    node->constant.type = CONST_INT;
    node->constant.value.int_val = value;
    node->is_constant = true;
    node->const_val.int_val = value;
    return node;
}

ASTNode* create_float_constant(ASTContext* ctx, float value, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_CONSTANT, loc);
    node->constant.type = CONST_FLOAT;
    node->constant.value.float_val = value;
    node->is_constant = true;
    node->const_val.float_val = value;
    return node;
}

ASTNode* create_array_init(ASTContext* ctx, ASTNode** elements, size_t elem_count, SourceLocation loc) {
    assert(ctx);
    ASTNode* node = create_node(ctx, AST_ARRAY_INIT, loc);
    node->array_init.elements = elements;
    node->array_init.elem_count = elem_count;
    set_parent_for_list(node, elements, elem_count);
    return node;
}

ASTNode* create_string_literal(ASTContext* ctx, const char* value, size_t length, SourceLocation loc) {
    assert(ctx && value);
    ASTNode* node = create_node(ctx, AST_STRING_LITERAL, loc);
    node->string_literal.value = pool_strdup(ctx->pool, value);
    node->string_literal.length = length;
    return node;
}

// ================================
// 5. 运行时库函数支持 (Library Function Support)
// ================================

LibraryFunction get_library_function_id(const char* name) {
    if (name == NULL) return LIB_UNKNOWN;
    // 使用静态映射表，提高查找效率
    static const struct {
        const char* name;
        LibraryFunction id;
    } lib_map[] = {
        {"getint",    LIB_GETINT},    {"getch",     LIB_GETCH},
        {"getfloat",  LIB_GETFLOAT},  {"getarray",  LIB_GETARRAY},
        {"getfarray", LIB_GETFARRAY}, {"putint",    LIB_PUTINT},
        {"putch",     LIB_PUTCH},     {"putfloat",  LIB_PUTFLOAT},
        {"putarray",  LIB_PUTARRAY},  {"putfarray", LIB_PUTFARRAY},
        {"putf",      LIB_PUTF},      {"starttime", LIB_STARTTIME},
        {"stoptime",  LIB_STOPTIME},
        {NULL,        LIB_UNKNOWN} // 哨兵值
    };

    for (int i = 0; lib_map[i].name; i++) {
        if (strcmp(name, lib_map[i].name) == 0) {
            return lib_map[i].id;
        }
    }
    return LIB_UNKNOWN;
}

// ================================
// 6. AST调试与打印 (AST Debugging & Printing)
// ================================

/** @brief 打印类型信息，用于调试。*/
void print_type(Type* type);

/** @brief 打印指定级别的缩进。*/
static void print_indent(int level) {
    for (int i = 0; i < level; ++i) printf("  ");
}

void print_ast(const ASTNode* node, int indent) {
    if (!node) return;
    print_indent(indent);
    printf("%s", ast_node_type_to_string(node->node_type));
    switch (node->node_type) {
        case AST_VAR_DECL:
            printf(" (%s)", node->var_decl.name);
            break;
        case AST_CONST_DECL:
            printf(" (%s)", node->const_decl.name);
            break;
        case AST_FUNC_DECL:
            printf(" (%s)", node->func_decl.func_name);
            break;
        case AST_FUNC_PARAM:
            printf(" (%s)", node->func_param.name);
            break;
        case AST_IDENTIFIER:
            printf(" (%s)", node->identifier.name);
            break;
        case AST_CONSTANT:
            if (node->constant.type == CONST_INT) {
                printf(" (%d)", node->constant.value.int_val);
            } else {
                printf(" (%f)", node->constant.value.float_val);
            }
            break;
        case AST_BINARY_EXPR:
            printf(" (%s)", operator_type_to_string(node->binary_expr.op));
            break;
        case AST_UNARY_EXPR:
            printf(" (%s)", operator_type_to_string(node->unary_expr.op));
            break;
        default:
            break;
    }
    printf("\n");
    // 递归打印子节点
    switch (node->node_type) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < node->compound_stmt.item_count; ++i) {
                print_ast(node->compound_stmt.items[i], indent + 1);
            }
            break;
        case AST_VAR_DECL:
            if (node->var_decl.init_value) {
                print_ast(node->var_decl.init_value, indent + 1);
            }
            if (node->var_decl.var_type && node->var_decl.var_type->kind == TYPE_ARRAY) {
                print_indent(indent + 1);
                printf("Array (size: %d)\n", node->var_decl.var_type->array.dimensions[0].static_size);
            }
            break;
        case AST_CONST_DECL:
            if (node->const_decl.value) {
                print_ast(node->const_decl.value, indent + 1);
            }
            break;
        case AST_FUNC_DECL:
            for (size_t i = 0; i < node->func_decl.param_count; ++i) {
                print_ast(node->func_decl.params[i], indent + 1);
            }
            print_ast(node->func_decl.body, indent + 1);
            break;
        case AST_IF_STMT:
            print_ast(node->if_stmt.cond, indent + 1);
            print_ast(node->if_stmt.then_stmt, indent + 1);
            if (node->if_stmt.else_stmt) {
                print_ast(node->if_stmt.else_stmt, indent + 1);
            }
            break;
        case AST_WHILE_STMT:
            print_ast(node->while_stmt.cond, indent + 1);
            print_ast(node->while_stmt.body, indent + 1);
            break;
        case AST_RETURN_STMT:
            if (node->return_stmt.value) {
                print_ast(node->return_stmt.value, indent + 1);
            }
            break;
        case AST_EXPR_STMT:
            if (node->expr_stmt.expr) {
                print_ast(node->expr_stmt.expr, indent + 1);
            }
            break;
        case AST_ASSIGN_STMT:
            print_ast(node->assign_stmt.lval, indent + 1);
            print_ast(node->assign_stmt.expr, indent + 1);
            break;
        case AST_CALL_EXPR:
            print_ast(node->call_expr.callee_expr, indent + 1);
            for (size_t i = 0; i < node->call_expr.arg_count; ++i) {
                print_ast(node->call_expr.args[i], indent + 1);
            }
            break;
        case AST_ARRAY_ACCESS:
            print_ast(node->array_access.array, indent + 1);
            print_ast(node->array_access.index, indent + 1);
            break;
        case AST_ARRAY_INIT:
            for (size_t i = 0; i < node->array_init.elem_count; ++i) {
                print_ast(node->array_init.elements[i], indent + 1);
            }
            break;
        default:
            // 对于没有子节点的节点，不执行任何操作
            break;
    }
}

const char* ast_node_type_to_string(ASTNodeType type) {
    static const char* const type_names[] = {
        [AST_VAR_DECL] = "VarDecl", [AST_CONST_DECL] = "ConstDecl", [AST_FUNC_DECL] = "FuncDecl",
        [AST_FUNC_PARAM] = "Param", [AST_COMPOUND_STMT] = "CompoundStmt", [AST_IF_STMT] = "IfStmt",
        [AST_WHILE_STMT] = "WhileStmt", [AST_RETURN_STMT] = "ReturnStmt", [AST_BREAK_STMT] = "BreakStmt",
        [AST_CONTINUE_STMT] = "ContinueStmt", [AST_EXPR_STMT] = "ExprStmt", [AST_ASSIGN_STMT] = "AssignStmt",
        [AST_BINARY_EXPR] = "BinaryExpr", [AST_UNARY_EXPR] = "UnaryExpr", [AST_CALL_EXPR] = "CallExpr",
        [AST_ARRAY_ACCESS] = "ArrayAccess", [AST_IDENTIFIER] = "Identifier", [AST_CONSTANT] = "Constant",
        [AST_STRING_LITERAL] = "StringLiteral", [AST_ARRAY_INIT] = "ArrayInit",
    };
    if (type >= 0 && type < (int)(sizeof(type_names)/sizeof(type_names[0])) && type_names[type] != NULL) {
        return type_names[type];
    }
    return "UnknownNode";
}

const char* operator_type_to_string(OperatorType op) {
    static const char* const op_names[] = {
        [OP_ADD] = "+", [OP_SUB] = "-", [OP_MUL] = "*", [OP_DIV] = "/", [OP_MOD] = "%",
        [OP_EQ] = "==", [OP_NE] = "!=", [OP_LT] = "<", [OP_GT] = ">", [OP_LE] = "<=", [OP_GE] = ">=",
        [OP_AND] = "&&", [OP_OR] = "||", [OP_POS] = "+", [OP_NEG] = "-", [OP_NOT] = "!",
    };
    if (op >= 0 && op < (int)(sizeof(op_names)/sizeof(op_names[0])) && op_names[op] != NULL) {
        return op_names[op];
    }
    return "?";
}

void print_type(Type* type) {
    if (!type) {
        printf("<null type>");
        return;
    }
    if (type->is_const) printf("const ");
    switch(type->kind) {
        case TYPE_VOID: printf("void"); break;
        case TYPE_BASIC:
            switch (type->basic) {
                case BASIC_INT: printf("i32"); break;
                case BASIC_FLOAT: printf("float"); break;
                case BASIC_I1: printf("i1"); break;
                case BASIC_I8: printf("i8"); break;
                case BASIC_I64: printf("i64"); break;
                case BASIC_DOUBLE: printf("double"); break;
            }
            break;
        case TYPE_ARRAY:
            print_type(type->array.element_type);
            for (size_t i = 0; i < type->array.dim_count; ++i) {
                if (type->array.dimensions[i].is_dynamic) {
                    printf("[]");
                } else {
                    printf("[%d]", type->array.dimensions[i].static_size);
                }
            }
            break;
        case TYPE_POINTER:
            print_type(type->pointer.element_type);
            printf("*");
            break;
        case TYPE_FUNCTION:
            print_type(type->function.return_type);
            printf("(");
            for (size_t i = 0; i < type->function.param_count; ++i) {
                print_type(type->function.param_types[i]);
                if (i < type->function.param_count - 1) printf(", ");
            }
            if (type->function.is_variadic) printf(", ...");
            printf(")");
            break;
    }
}

// ================================
// 7. 辅助函数 (Helper Functions)
// ================================

/**
 * @brief 设置子节点的父节点指针。
 * @details 建立父子关系对于AST的向上遍历（例如，在语义分析中查找上下文信息）至关重要。
 * @param parent 父节点。
 * @param child 子节点，可以为NULL。
 */
static void set_parent(ASTNode* parent, ASTNode* child) {
    if (child) {
        child->parent = parent;
    }
}

/**
 * @brief 为一个节点列表中的所有节点设置父节点。
 * @param parent 父节点。
 * @param list 节点指针数组，可以为NULL。
 * @param count 列表中的节点数量。
 */
static void set_parent_for_list(ASTNode* parent, ASTNode** list, size_t count) {
    if (list) {
        for (size_t i = 0; i < count; i++) {
            if (list[i]) {
                list[i]->parent = parent;
            }
        }
    }
}