/**
 * @file ast.h
 * @brief 抽象语法树（AST）核心定义头文件
 * @details
 * 本文件定义了编译器前端所需的所有核心AST数据结构。
 * 它包括：
 * 1.  **类型系统**：用于表示语言中的各种类型（如int、float、数组、函数等）。
 * 2.  **常量值**：用于表示编译时常量。
 * 3.  **AST节点**：定义了所有语法结构对应的节点类型和数据结构。
 * 4.  **AST上下文**：管理AST的全局状态，如内存池和错误报告。
 * 5.  **公共API**：提供了创建和管理类型及AST节点的工厂函数。
 */

#ifndef AST_H
#define AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "error.h"
#include "location.h"
#include "logger.h"

// 前向声明，用于解决头文件中的循环依赖问题
// 这些结构体在别处定义，此处仅声明其存在，以便在指针中使用
typedef struct MemoryPool MemoryPool;
typedef struct ASTNode ASTNode;
typedef struct SymbolTable SymbolTable;
typedef struct Symbol Symbol;
typedef struct Type Type;

// ================================
// 1. 类型系统 (Type System)
// ================================

/**
 * @enum BasicType
 * @brief 基础数据类型枚举。
 */
typedef enum {
    BASIC_INT,      ///< 整型 (i32)
    BASIC_FLOAT,    ///< 浮点型 (f32)
    BASIC_I1,       ///< 1位布尔类型（LLVM风格）
    BASIC_I8,       ///< 8位整型 (i8, 用于char/byte)
    BASIC_I64,      ///< 64位整型 (i64)
    BASIC_DOUBLE    ///< 双精度浮点型 (f64)
} BasicType;

/**
 * @enum TypeKind
 * @brief 描述一个类型的种类。
 */
typedef enum {
    TYPE_VOID,      ///< void 类型
    TYPE_BASIC,     ///< 基础类型 (int, float)
    TYPE_ARRAY,     ///< 数组类型
    TYPE_POINTER,   ///< 指针类型 (编译器内部使用)
    TYPE_FUNCTION   ///< 函数类型
} TypeKind;

/**
 * @struct ArrayDimension
 * @brief 描述数组的单个维度。
 */
typedef struct {
    bool is_dynamic;      ///< 是否是动态维度（例如 `int a[]`）
    int static_size;      ///< 如果是静态维度，其大小
    ASTNode* dim_expr;    ///< 维度表达式，用于后续求值（如 `int a[N+1]`）
} ArrayDimension;

/**
 * @struct Type
 * @brief 核心类型描述结构体。
 * @details 使用联合体(union)来表示不同种类的类型信息。
 */
struct Type {
    TypeKind kind;      ///< 类型的种类
    bool is_const;      ///< 类型是否带有const限定符

    union {
        /// @brief 基础类型信息
        BasicType basic;

        /// @brief 数组类型信息
        struct {
            Type* element_type;         ///< 数组元素的类型
            size_t dim_count;           ///< 数组的维度数量
            ArrayDimension* dimensions; ///< 各个维度的详细信息
        } array;

        /// @brief 指针类型信息
        struct {
            Type* element_type;         ///< 指针指向的元素类型
        } pointer;

        /// @brief 函数类型信息
        struct {
            Type* return_type;          ///< 函数的返回类型
            Type** param_types;         ///< 参数类型列表
            size_t param_count;         ///< 参数数量
            bool is_variadic;           ///< 是否为可变参数函数
        } function;
    };
};

// ================================
// 2. 常量值 (Constant Values)
// ================================

/**
 * @enum ConstantKind
 * @brief 常量值的类型枚举。
 */
typedef enum {
    CONST_INT,      ///< 整型常量
    CONST_FLOAT     ///< 浮点型常量
} ConstantKind;

/**
 * @union ConstValueUnion
 * @brief 用于存储编译时常量值的联合体。
 */
typedef union {
    int32_t int_val;    ///< 32位整型值
    float float_val;    ///< 单精度浮点值
} ConstValueUnion;

// ================================
// 3. AST节点定义 (AST Node Definitions)
// ================================

/**
 * @enum ASTNodeType
 * @brief 所有AST节点类型的枚举。
 */
typedef enum {
    // 声明 (Declarations)
    AST_VAR_DECL,           ///< 变量声明
    AST_CONST_DECL,         ///< 常量声明
    AST_FUNC_DECL,          ///< 函数声明
    AST_FUNC_PARAM,         ///< 函数参数

    // 语句 (Statements)
    AST_COMPOUND_STMT,      ///< 复合语句 (代码块)
    AST_EXPR_STMT,          ///< 表达式语句
    AST_IF_STMT,            ///< if-else 语句
    AST_WHILE_STMT,         ///< while 循环语句
    AST_ASSIGN_STMT,        ///< 赋值语句
    AST_RETURN_STMT,        ///< return 语句
    AST_BREAK_STMT,         ///< break 语句
    AST_CONTINUE_STMT,      ///< continue 语句

    // 表达式 (Expressions)
    AST_BINARY_EXPR,        ///< 二元表达式
    AST_UNARY_EXPR,         ///< 一元表达式
    AST_CALL_EXPR,          ///< 函数调用表达式
    AST_ARRAY_ACCESS,       ///< 数组访问表达式

    // 字面量与标识符 (Literals & Identifiers)
    AST_IDENTIFIER,         ///< 标识符
    AST_CONSTANT,           ///< 常量 (整型/浮点型)
    AST_STRING_LITERAL,     ///< 字符串字面量
    AST_ARRAY_INIT,         ///< 数组初始化列表
    AST_NODE_TYPE_COUNT     ///< 节点类型总数（用于范围检查）
} ASTNodeType;

/**
 * @enum OperatorType
 * @brief 表达式中操作符的类型枚举。
 */
typedef enum {
    // 二元算术运算符
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    // 二元关系运算符
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    // 二元逻辑运算符
    OP_AND, OP_OR,
    // 一元运算符
    OP_POS, OP_NEG, OP_NOT
} OperatorType;

// --- 各AST节点的专用数据结构 ---
// 注意：这些结构体是ASTNode中联合体的成员，用于存储特定节点类型的数据。

typedef struct { char* name; Type* var_type; ASTNode* init_value; } VarDeclNode;
typedef struct { char* name; Type* const_type; ASTNode* value; } ConstDeclNode;
typedef struct { char* func_name; Type* return_type; size_t param_count; ASTNode** params; ASTNode* body; SymbolTable* scope; } FuncDeclNode;
typedef struct { char* name; Type* param_type; } FuncParamNode;
typedef struct { size_t item_count; ASTNode** items; SymbolTable* scope; } CompoundStmtNode;
typedef struct { ASTNode* lval; ASTNode* expr; } AssignStmtNode;
typedef struct { ASTNode* expr; } ExprStmtNode;
typedef struct { ASTNode* cond; ASTNode* then_stmt; ASTNode* else_stmt; } IfStmtNode;
typedef struct { ASTNode* cond; ASTNode* body; } WhileStmtNode;
typedef struct { ASTNode* value; } ReturnStmtNode;
typedef struct { OperatorType op; ASTNode* left; ASTNode* right; } BinaryExprNode;
typedef struct { OperatorType op; ASTNode* operand; } UnaryExprNode;
typedef struct { ASTNode* callee_expr; size_t arg_count; ASTNode** args; } CallExprNode;
typedef struct { ASTNode* array; ASTNode* index; } ArrayAccessNode;
typedef struct { char* name; } IdentifierNode;
typedef struct { ConstantKind type; ConstValueUnion value; } ConstantNode;
typedef struct { char* value; size_t length; } StringLiteralNode;
typedef struct { size_t elem_count; ASTNode** elements; } ArrayInitNode;

/**
 * @struct ASTNode
 * @brief AST的核心节点结构体。
 * @details 这是一个通用的节点结构，通过`node_type`区分其具体类型，
 *          并使用联合体存储不同类型节点的特定数据。
 */
struct ASTNode {
    ASTNodeType node_type;      ///< 节点类型
    SourceLocation loc;         ///< 节点在源代码中的位置信息
    Type* eval_type;            ///< 表达式求值后的类型
    struct ASTNode* parent;     ///< 指向父节点的指针，便于上下文分析
    bool is_lvalue;             ///< 表达式是否为左值
    bool is_constant;           ///< 表达式是否为编译时常量
    ConstValueUnion const_val;  ///< 如果是编译时常量，其值
    Symbol* sym;                ///< 指向符号表中对应符号的链接

    union {
        VarDeclNode var_decl;
        ConstDeclNode const_decl;
        FuncDeclNode func_decl;
        FuncParamNode func_param;
        CompoundStmtNode compound_stmt;
        AssignStmtNode assign_stmt;
        ExprStmtNode expr_stmt;
        IfStmtNode if_stmt;
        WhileStmtNode while_stmt;
        ReturnStmtNode return_stmt;
        BinaryExprNode binary_expr;
        UnaryExprNode unary_expr;
        CallExprNode call_expr;
        ArrayAccessNode array_access;
        IdentifierNode identifier;
        ConstantNode constant;
        StringLiteralNode string_literal;
        ArrayInitNode array_init;
    };
};

/**
 * @struct ASTNodeList
 * @brief 一个辅助结构体，用于在解析过程中动态构建AST节点列表。
 */
typedef struct ASTNodeList {
    ASTNode** items;
    size_t count;
    size_t capacity;
} ASTNodeList;

// ================================
// 4. AST上下文与库函数
// ================================

/**
 * @enum LibraryFunction
 * @brief 内置库函数的ID枚举。
 */
typedef enum {
    LIB_UNKNOWN = -1,   ///< 未知或非库函数
    LIB_GETINT,         ///< getint()
    LIB_GETCH,          ///< getch()
    LIB_GETFLOAT,       ///< getfloat()
    LIB_GETARRAY,       ///< getarray()
    LIB_GETFARRAY,      ///< getfarray()
    LIB_PUTINT,         ///< putint()
    LIB_PUTCH,          ///< putch()
    LIB_PUTFLOAT,       ///< putfloat()
    LIB_PUTARRAY,       ///< putarray()
    LIB_PUTFARRAY,      ///< putfarray()
    LIB_PUTF,           ///< putf()
    LIB_STARTTIME,      ///< starttime()
    LIB_STOPTIME        ///< stoptime()
} LibraryFunction;

/**
 * @struct ASTContext
 * @brief AST的全局上下文。
 * @details 存储了整个编译单元的AST相关信息，如内存池、根节点、符号表等。
 */
typedef struct ASTContext {
    ASTNode* root;              ///< AST的根节点（通常是一个包含所有全局声明的虚拟节点）
    MemoryPool* pool;           ///< 用于分配AST节点和类型等对象的内存池
    ErrorContext errors;        ///< 错误收集和报告上下文（值类型，生命周期随ASTContext）
    SymbolTable* global_scope;  ///< 全局作用域的符号表
    char* source_filename;      ///< 源文件名
    bool has_main;              ///< 是否已发现main函数
    LogConfig log_config;       ///< 日志配置，用于控制日志输出
} ASTContext;


// ================================
// 5. 公共API声明
// ================================

// --- 内存池API (Memory Pool API) ---

/** @brief 创建一个新的内存池。 */
MemoryPool* create_memory_pool();
/** @brief 销毁一个内存池，释放其分配的所有内存。 */
void destroy_memory_pool(MemoryPool* pool);
/** @brief 从内存池中分配指定大小的内存。 */
void* pool_alloc(MemoryPool* pool, size_t size);
/** @brief 在内存池中复制一个字符串。 */
char* pool_strdup(MemoryPool* pool, const char* s);

// --- AST上下文API (AST Context API) ---

/** @brief 创建并初始化一个新的AST上下文。 */
ASTContext* create_ast_context();
/** @brief 销毁一个AST上下文，包括其内存池和相关资源。 */
void destroy_ast_context(ASTContext* ctx);

// --- 类型创建API (Type Creation API) ---

/** @brief 创建一个基础类型（int/float）。*/
Type* create_basic_type(BasicType basic, bool is_const, MemoryPool* pool);
/** @brief 创建一个数组类型。*/
Type* create_array_type(Type* element_type, ArrayDimension* dims, size_t dim_count, bool is_const, MemoryPool* pool);
/** @brief 创建一个指针类型。*/
Type* create_pointer_type(Type* element_type, bool is_const, MemoryPool* pool);
/** @brief 创建一个函数类型。*/
Type* create_function_type(Type* return_type, Type** param_types, size_t param_count, bool is_variadic, MemoryPool* pool);
/** @brief 从参数节点数组创建函数类型（优化版本，避免中间内存分配）。*/
Type* create_function_type_from_params(Type* return_type, ASTNode** param_nodes, size_t param_count, bool is_variadic, MemoryPool* pool);
/** @brief 创建一个void类型。*/
Type* create_void_type(MemoryPool* pool);

// --- AST节点创建API (AST Node Creation API) ---
// 这些函数是构建AST的工厂函数，它们从AST上下文中获取内存并初始化节点。

ASTNode* create_var_decl(ASTContext* ctx, const char* name, const Type* type, const ASTNode* init, SourceLocation loc);
ASTNode* create_const_decl(ASTContext* ctx, const char* name, const Type* type, const ASTNode* value, SourceLocation loc);
ASTNode* create_func_param(ASTContext* ctx, const char* name, const Type* type, SourceLocation loc);
ASTNode* create_func_decl(ASTContext* ctx, const char* name, const Type* return_type, ASTNode** params, size_t param_count, ASTNode* body, SourceLocation loc);
ASTNode* create_compound_stmt(ASTContext* ctx, ASTNode** items, size_t count, SourceLocation loc);
ASTNode* create_if_stmt(ASTContext* ctx, const ASTNode* cond, const ASTNode* then_stmt, const ASTNode* else_stmt, SourceLocation loc);
ASTNode* create_while_stmt(ASTContext* ctx, const ASTNode* cond, const ASTNode* body, SourceLocation loc);
ASTNode* create_return_stmt(ASTContext* ctx, const ASTNode* value, SourceLocation loc);
ASTNode* create_break_stmt(ASTContext* ctx, SourceLocation loc);
ASTNode* create_continue_stmt(ASTContext* ctx, SourceLocation loc);
ASTNode* create_expr_stmt(ASTContext* ctx, const ASTNode* expr, SourceLocation loc);
ASTNode* create_assign_stmt(ASTContext* ctx, const ASTNode* lval, const ASTNode* expr, SourceLocation loc);
ASTNode* create_binary_expr(ASTContext* ctx, OperatorType op, const ASTNode* left, const ASTNode* right, SourceLocation loc);
ASTNode* create_unary_expr(ASTContext* ctx, OperatorType op, const ASTNode* operand, SourceLocation loc);
ASTNode* create_call_expr(ASTContext* ctx, ASTNode* callee_expr, ASTNode** args, size_t arg_count, SourceLocation loc);
ASTNode* create_array_access(ASTContext* ctx, const ASTNode* array, const ASTNode* index, SourceLocation loc);
ASTNode* create_identifier(ASTContext* ctx, const char* name, SourceLocation loc);
ASTNode* create_int_constant(ASTContext* ctx, int value, SourceLocation loc);
ASTNode* create_float_constant(ASTContext* ctx, float value, SourceLocation loc);
ASTNode* create_array_init(ASTContext* ctx, ASTNode** elements, size_t elem_count, SourceLocation loc);
ASTNode* create_string_literal(ASTContext* ctx, const char* value, size_t length, SourceLocation loc);

// --- 辅助函数 (Utility Functions) ---

/** @brief 将AST节点类型枚举转换为可读的字符串。*/
const char* ast_node_type_to_string(ASTNodeType type);

/**
 * @brief 根据函数名获取其对应的库函数ID。
 * @param name 函数名。
 * @return 如果是已知的库函数，返回其ID；否则返回 `LIB_UNKNOWN`。
 */
LibraryFunction get_library_function_id(const char* name);

/**
 * @brief 递归打印AST，用于调试。
 * @param node 要打印的AST节点。
 * @param indent_level 打印的缩进级别。
 */
void print_ast(const ASTNode* node, int indent_level);

#endif // AST_H