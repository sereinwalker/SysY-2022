#ifndef ERROR_H
#define ERROR_H

#include "location.h" // 包含源代码位置跟踪的定义
#include <stddef.h>
#include <stdbool.h>

/**
 * @file error.h
 * @brief 定义编译器中用于错误收集和报告的结构体和函数。
 * @details
 * 该模块提供了一个集中式的机制，用于记录从词法分析到语义分析乃至后续阶段
 * 发现的所有编译错误。通过在打印前收集所有错误，编译器可以在一次运行中报告多个问题，
 * 而不是在遇到第一个错误时就停止。
 *
 * 每个错误由一个 `ErrorEntry` 结构体表示，其中包括：
 * - 一个类型 (`ErrorType`) 用于分类。
 * - 一个严重级别 (`ErrorSeverity`) 用于区分优先级。
 * - 一条描述性的消息。
 * - 它在源代码中的精确位置 (`SourceLocation`)。
 *
 * 所有错误都存储在一个 `ErrorContext` 内部的动态伸缩数组中。
 */

/**
 * @enum ErrorSeverity
 * @brief 定义错误分类的严重级别。
 * @details
 * 这些级别有助于区分致命错误、普通错误、警告和信息性消息，
 * 从而对错误进行优先级排序并提供更好的用户体验。
 */
typedef enum {
    ERROR_SEVERITY_INFO,       ///< 信息性消息，不属于错误
    ERROR_SEVERITY_WARNING,    ///< 不会阻止编译的警告
    ERROR_SEVERITY_ERROR,      ///< 会导致编译失败的错误
    ERROR_SEVERITY_FATAL       ///< 需要立即终止的致命错误
} ErrorSeverity;

/**
 * @enum ErrorType
 * @brief 定义编译器可以报告的所有可能的错误类别。
 * @details
 * 这些枚举值为错误分类提供了一种结构化的方式，这对于更复杂的错误处理、
 * 过滤或生成编译统计数据非常有用。
 */
typedef enum {
    ERROR_NONE = 0,
    ERROR_MEMORY_ALLOCATION,        ///< 致命的内存分配失败（例如 malloc/realloc 失败）
    ERROR_LEXICAL,                  ///< 词法错误（例如非法字符、未闭合的注释/字符串）
    ERROR_SYNTAX,                   ///< 语法错误（由 Bison 报告的解析错误）
    ERROR_SEMANTIC,                 ///< 语义错误
    ERROR_DUPLICATE_SYMBOL,         ///< 符号重定义（例如在同一作用域内重复的变量或函数名）
    ERROR_UNDEFINED_VARIABLE,       ///< 使用了未声明的变量或函数
    ERROR_TYPE_MISMATCH,            ///< 操作中的类型不兼容（例如 `int a = 1.0 + "hello";`）
    ERROR_INVALID_CONVERSION,       ///< 非法的类型转换（例如将一个非常量赋值给 `const` 变量）
    ERROR_NOT_LVALUE,               ///< 赋值操作的左侧不是一个可修改的左值
    ERROR_INVALID_PARAMETER,        ///< 函数调用中的参数数量或类型不正确
    ERROR_MISSING_RETURN,           ///< 非 void 函数在某些执行路径上缺少 return 语句
    ERROR_MAIN_FUNCTION_MISSING,    ///< 缺少 'main' 函数，或其签名不正确
    ERROR_ARRAY_DIM_MISMATCH,       ///< 数组维度不匹配（例如在赋值或函数调用中）
    ERROR_INVALID_ARRAY_INIT,       ///< 数组初始化列表中的错误（例如元素过多、类型不匹配、维度不是常量）
    ERROR_INVALID_ARRAY_ACCESS,     ///< 对非数组变量使用下标，或使用非整数索引
    ERROR_BREAK_CONTINUE_OUTSIDE_LOOP, ///< 在循环结构之外使用了 'break' 或 'continue' 语句
    ERROR_LIBRARY_FUNCTION_MISUSE,  ///< 库函数使用不当（例如将字符串传递给 `putint`）
    ERROR_FORMAT_STRING_INVALID,    ///< `putf` 的格式化字符串无效（例如非法的说明符如 `%z`）
    ERROR_IR_GENERATION,            ///< IR 生成阶段的错误
    ERROR_IR_OPTIMIZATION,          ///< IR 优化阶段的错误
    ERROR_BACKEND_GENERATION,       ///< 后端代码生成阶段的错误
    ERROR_RUNTIME,                  ///< 运行时错误
    ERROR_INVALID_ASSIGNMENT,       ///< 无效的赋值操作
    ERROR_INVALID_CONTROL_FLOW,     ///< 无效的控制流操作
    ERROR_MISSING_INITIALIZER       ///< 缺少初始化器（如 const/数组/变量声明未初始化）
} ErrorType;

/**
 * @struct ErrorEntry
 * @brief 表示单个编译错误的条目。
 * @details
 * 该结构体捕获了向用户报告单个特定错误所需的所有信息。
 */
typedef struct {
    ErrorType type;            ///< 错误的类别
    ErrorSeverity severity;    ///< 错误的严重级别
    char message[256];         ///< 用于存储具体的、人类可读的错误消息的缓冲区
    SourceLocation loc;        ///< 错误在源代码中的位置（行和列）
} ErrorEntry;

/**
 * @struct ErrorContext
 * @brief 用于收集所有编译错误的上下文。
 * @details
 * 该结构体管理一个动态数组，用于存储在整个编译过程中发现的所有 `ErrorEntry` 实例。
 * 它可以根据需要增长以容纳更多错误。
 */
// 注释说明 ErrorContext 典型用法为值类型，推荐自动管理
typedef struct ErrorContext {
    ErrorEntry* errors;        ///< 指向动态分配的错误条目数组的指针
    size_t count;              ///< 数组中当前记录的错误数量
    size_t capacity;           ///< `errors` 数组当前分配的容量
    bool has_fatal_errors;     ///< 标记是否已记录任何致命错误
} ErrorContext;

// --- 错误严重级别映射函数 ---

/**
 * @brief 将错误类型映射到其对应的严重级别。
 * @param type 要映射的错误类型。
 * @return 对应错误类型的严重级别。
 */
ErrorSeverity get_error_severity(ErrorType type);

/**
 * @brief 获取错误严重级别的人类可读的字符串表示。
 * @param severity 要转换的严重级别。
 * @return 严重级别的字符串表示。
 */
const char* get_error_severity_string(ErrorSeverity severity);

/**
 * @brief 获取错误类型的人类可读的字符串表示。
 * @param type 要转换的错误类型。
 * @return 错误类型的字符串表示。
 */
const char* get_error_type_string(ErrorType type);

// --- 参数验证函数 ---

/**
 * @brief 验证错误上下文参数的安全性。
 * @param ctx 要验证的错误上下文。
 * @param type 要验证的错误类型。
 * @param msg 要验证的错误消息。
 * @return 如果所有参数都有效，则返回 true，否则返回 false。
 */
bool validate_error_parameters(const ErrorContext* ctx, ErrorType type, const char* msg);

/**
 * @brief 验证源位置参数的安全性。
 * @param loc 要验证的源位置。
 * @return 如果位置有效，则返回 true，否则返回 false。
 */
bool validate_source_location(const SourceLocation* loc);

// --- 增强的错误上下文 API ---

/**
 * @brief 初始化错误上下文，为其分配初始内存。
 * @details
 * 此函数必须在添加任何错误之前调用。它会执行参数验证并返回成功状态。
 * @param ctx 指向待初始化的 `ErrorContext` 实例的指针。
 * @param initial_capacity 错误数组的初始容量。一个较小的值（如16）通常就足够了。
 * @return 如果初始化成功，则返回 true，否则返回 false。
 */
bool init_error_context(ErrorContext* ctx, size_t initial_capacity);

/**
 * @brief 向错误上下文中添加一条新的错误记录。
 * @details
 * 如果当前容量不足，此函数将自动扩展内部数组。提供的消息会被安全地复制到内部缓冲区。
 * 执行全面的参数验证。
 * @param ctx 指向将要添加错误的 `ErrorContext` 实例的指针。
 * @param type 错误的类型，来自 `ErrorType` 枚举。
 * @param msg 描述错误的具体字符串消息。
 * @param loc 错误发生的 `SourceLocation`。
 * @return 如果错误成功添加，则返回 true，否则返回 false。
 */
bool add_error(ErrorContext* ctx, ErrorType type, const char* msg, SourceLocation loc);

/**
 * @brief 添加一条具有显式严重级别的新错误记录。
 * @details
 * 此函数允许显式控制严重级别，这在默认严重级别映射不适用的情况下很有用。
 * @param ctx 指向将要添加错误的 `ErrorContext` 实例的指针。
 * @param type 错误的类型，来自 `ErrorType` 枚举。
 * @param severity 此错误的显式严重级别。
 * @param msg 描述错误的具体字符串消息。
 * @param loc 错误发生的 `SourceLocation`。
 * @return 如果错误成功添加，则返回 true，否则返回 false。
 */
bool add_error_with_severity(ErrorContext* ctx, ErrorType type, ErrorSeverity severity,
                           const char* msg, SourceLocation loc);

/**
 * @brief 释放错误上下文占用的所有内存。
 * @details
 * 这包括持有所有错误条目的动态数组。此调用后，该上下文将不再有效。
 * 执行空指针验证。
 * @param ctx 指向待释放的 `ErrorContext` 实例的指针。
 */
void free_error_context(ErrorContext* ctx);

/**
 * @brief 检查错误上下文是否包含任何致命错误。
 * @param ctx 要检查的错误上下文。
 * @return 如果存在致命错误，则返回 true，否则返回 false。
 */
bool has_fatal_errors(const ErrorContext* ctx);

/**
 * @brief 按严重级别获取错误数量。
 * @param ctx 要查询的错误上下文。
 * @param severity 要计数的严重级别。
 * @return 具有指定严重级别的错误数量。
 */
size_t get_error_count_by_severity(const ErrorContext* ctx, ErrorSeverity severity);

/**
 * @brief 获取总的错误数量（不包括信息性消息）。
 * @param ctx 要查询的错误上下文。
 * @return 错误和警告的总数。
 */
size_t get_total_error_count(const ErrorContext* ctx);

#endif // ERROR_H