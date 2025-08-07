#include "symbol_table.h"
#include <string.h>
#include <assert.h>

/**
 * @file symbol_table.c
 * @brief 实现了符号表数据结构和API。
 * @details
 * 本文件提供了 symbol_table.h 中声明的函数的具体实现，
 * 包括创建、销毁、添加和查找符号等功能。
 */

// 符号表哈希桶的初始容量
#define INITIAL_CAPACITY 16

/**
 * @brief 计算字符串的哈希值。
 * @details 使用 djb2 哈希算法，这是一种简单而高效的非加密哈希函数。
 * @param str 要计算哈希值的输入字符串。
 * @return 字符串的无符号长整型哈希值。
 */
static unsigned long hash_function(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

SymbolTable* create_symbol_table(ASTContext* ctx, SymbolTable* parent) {
    assert(ctx != NULL && "创建符号表时 ASTContext 不能为空");
    // 从内存池分配符号表结构体
    SymbolTable* table = (SymbolTable*)pool_alloc(ctx->pool, sizeof(SymbolTable));
    
    table->capacity = INITIAL_CAPACITY;
    table->count = 0;
    table->parent = parent;
    
    // 从内存池分配哈希桶数组
    table->buckets = (Symbol**)pool_alloc(ctx->pool, table->capacity * sizeof(Symbol*));
    // 将所有桶的头指针初始化为 NULL
    for (size_t i = 0; i < table->capacity; ++i) {
        table->buckets[i] = NULL;
    }

    return table;
}

Symbol* find_symbol_in_scope(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;

    // 计算哈希索引
    unsigned long index = hash_function(name) % table->capacity;
    // 遍历对应桶中的链表
    for (Symbol* symbol = table->buckets[index]; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) {
            return symbol; // 找到符号
        }
    }
    return NULL; // 在当前作用域未找到
}

Symbol* find_symbol(SymbolTable* table, const char* name) {
    if (!name) return NULL;
    // 沿着作用域链向上查找符号
    for (SymbolTable* scope = table; scope != NULL; scope = scope->parent) {
        Symbol* sym = find_symbol_in_scope(scope, name);
        if (sym) {
            return sym; // 在某个作用域中找到符号
        }
    }
    return NULL; // 所有作用域都未找到
}

bool add_symbol(SymbolTable* table, const char* name, Type* type, bool is_func, bool is_const, ASTContext* ctx) {
    assert(table != NULL && "FATAL: add_symbol 调用时 table 为 NULL。");
    assert(ctx != NULL && "FATAL: add_symbol 调用时 ASTContext 为 NULL。");
    
    // 仅在 *当前* 作用域检查是否重定义
    if (find_symbol_in_scope(table, name)) {
        return false; // 符号已存在
    }

    // 从内存池分配新符号
    Symbol* symbol = (Symbol*)pool_alloc(ctx->pool, sizeof(Symbol));
    
    symbol->name = pool_strdup(ctx->pool, name); // 复制符号名到内存池
    symbol->type = type;
    symbol->is_func = is_func;
    symbol->is_const = is_const;
    symbol->is_evaluated = false;
    memset(&symbol->const_val, 0, sizeof(symbol->const_val));

    // 将新符号添加到哈希链的头部
    unsigned long index = hash_function(name) % table->capacity;
    symbol->next = table->buckets[index];
    table->buckets[index] = symbol;
    table->count++;
    
    return true; // 添加成功
}

/**
 * @brief 判断符号表是否为空。
 * @param table 要检查的符号表。
 * @return 如果符号表存在且符号数量为0，则返回 true。
 */
bool is_symbol_table_empty(SymbolTable* table) {
    return table && table->count == 0;
}

/**
 * @brief 获取符号表中的符号数量。
 * @param table 要计数的符号表。
 * @return 如果符号表存在，返回其符号数量，否则返回0。
 */
size_t get_symbol_count(SymbolTable* table) {
    return table ? table->count : 0;
}

/**
 * @brief 更新符号的常量值。
 * @param symbol 要更新的符号。
 * @param value 新的常量值。
 */
void update_symbol_constant_value(Symbol* symbol, ConstValueUnion value) {
    if (symbol) {
        symbol->const_val = value;
        symbol->is_evaluated = true;
    }
}

/**
 * @brief 销毁符号表。
 * @details 由于所有内存都由内存池管理，此函数实际上是一个空操作（no-op）。
 *          内存将在 `destroy_ast_context` 中随内存池一起被释放。
 *          保留此函数是为了API的完整性和未来的扩展性。
 * @param table 要“销毁”的符号表。
 * @param ctx AST 上下文。
 */
void destroy_symbol_table(SymbolTable* table, ASTContext* ctx) {
    // 空操作：内存池将一次性释放所有内存
    (void)table;
    (void)ctx;
}