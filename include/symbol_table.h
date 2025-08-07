#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include "ast.h" // 包含了 Type, ASTNode 等的前向声明

/**
 * @file symbol_table.h
 * @brief 定义用于作用域管理的符号表数据结构和API。
 * @details
 * 符号表通过使用哈希表（采用链地址法解决冲突）实现了一个层级式的作用域系统。
 * 每个作用域都可以有一个父作用域，从而支持编程语言中的嵌套作用域。
 */

/**
 * @struct Symbol
 * @brief 表示作用域中一个已命名的实体（变量、常量或函数）。
 */
typedef struct Symbol {
    char* name;                ///< 符号的名称
    Type* type;                ///< 符号的类型
    bool is_func;              ///< 如果是函数符号，则为 true
    bool is_const;             ///< 如果是 const 变量或函数，则为 true
    bool is_evaluated;         ///< (仅用于常量) 如果其值已被计算，则为 true
    ConstValueUnion const_val; ///< (仅用于常量) 如果是已求值的常量，则为其编译时值
    struct Symbol* next;       ///< 用于哈希表链地址法的指针，指向下一个符号
} Symbol;

/**
 * @struct SymbolTable
 * @brief 表示一个单独的作用域。
 */
typedef struct SymbolTable {
    Symbol** buckets;           ///< 哈希表的桶数组，每个元素是指向 Symbol 链表头的指针
    size_t capacity;            ///< 哈希表的容量
    size_t count;               ///< 当前作用域中的符号数量
    struct SymbolTable* parent; ///< 指向外层作用域（父作用域）的链接
} SymbolTable;

// --- API 函数 ---

/**
 * @brief 创建一个新的符号表，并指定其父作用域。
 * @param ctx 用于内存分配的 AST 上下文。
 * @param parent 父作用域，对于全局作用域则为 NULL。
 * @return 指向新创建的符号表的指针。
 */
SymbolTable* create_symbol_table(ASTContext* ctx, SymbolTable* parent);

/**
 * @brief 销毁一个符号表并释放所有相关内存。
 * @param table 要销毁的符号表。
 * @param ctx 用于内存管理的 AST 上下文。
 */
void destroy_symbol_table(SymbolTable* table, ASTContext* ctx);

/**
 * @brief 向指定的符号表中添加一个新符号。
 * @param table 要添加符号的符号表。
 * @param name 符号的名称。
 * @param type 符号的类型。
 * @param is_func 如果符号代表一个函数，则为 true。
 * @param is_const 如果符号是常量，则为 true。
 * @param ctx 用于内存分配的 AST 上下文。
 * @return 如果符号已在此作用域中存在，则返回 false，否则返回 true。
 */
bool add_symbol(SymbolTable* table, const char* name, Type* type, bool is_func, bool is_const, ASTContext* ctx);

/**
 * @brief 仅在给定作用域中查找符号，不检查父作用域。
 * @param table 要搜索的符号表。
 * @param name 要查找的符号的名称。
 * @return 如果找到，则返回指向符号的指针，否则返回 NULL。
 */
Symbol* find_symbol_in_scope(SymbolTable* table, const char* name);

/**
 * @brief 通过搜索给定作用域及其所有父作用域来查找符号。
 * @param table 开始搜索的符号表。
 * @param name 要查找的符号的名称。
 * @return 如果找到，则返回指向符号的指针，否则返回 NULL。
 */
Symbol* find_symbol(SymbolTable* table, const char* name);

/**
 * @brief 更新符号的常量值。
 * @param symbol 要更新的符号。
 * @param value 要设置的常量值。
 */
void update_symbol_constant_value(Symbol* symbol, ConstValueUnion value);

/**
 * @brief 检查符号表是否为空。
 * @param table 要检查的符号表。
 * @return 如果表为空，则返回 true，否则返回 false。
 */
bool is_symbol_table_empty(SymbolTable* table);

/**
 * @brief 获取符号表中的符号数量。
 * @param table 要计数的符号表。
 * @return 表中的符号数量。
 */
size_t get_symbol_count(SymbolTable* table);

#endif // SYMBOL_TABLE_H