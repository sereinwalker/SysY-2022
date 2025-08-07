#ifndef SCANNER_CONTEXT_H
#define SCANNER_CONTEXT_H

#include <stdio.h>     // 为了 FILE*
#include "ast.h"       // 包含 AST 相关的定义
#include "location.h"  // 假设 location.h 包含 SourceLocation 结构体定义

// 这是一个“不透明指针”的前向声明。
// 真正的 `yyscan_t` 类型是由 Flex 在生成 lexer.yy.c 时内部定义的。
// 我们只需要知道它是一个指针类型（void*），就可以在函数签名中使用它。
typedef void* yyscan_t;

/**
 * @brief 统一的扫描器/解析器上下文结构体。
 * @details 
 * 这个结构体将所有解析过程中需要的状态信息打包在一起。
 * 在纯解析器（reentrant parser）模式下，这个结构体的实例会作为参数
 * 在驱动程序、Bison解析器和Flex词法分析器之间传递，从而避免使用任何全局变量。
 */
typedef struct ScannerContext {
    // --- 核心状态 ---

    /**
     * @brief 指向抽象语法树（AST）的上下文。
     * @details 包含用于节点分配的内存池和最终的AST根节点。
     */
    ASTContext* ast_ctx;

    /**
     * @brief 当前正在解析的文件名。
     * @details 用于在错误报告中提供清晰的来源信息。上下文会持有它自己的副本。
     */
    const char* filename;

    // --- 词法分析状态 (从 lexer.l 移入) ---

    /**
     * @brief 用于处理字符串字面量的动态缓冲区。
     * @details 当词法分析器遇到一个字符串时，它会把字符逐个存放在这里。
     */
    char* string_buffer;

    /**
     * @brief string_buffer 的当前分配大小。
     */
    size_t string_buffer_size;

    /**
     * @brief string_buffer 中当前已存入的字符数量。
     */
    size_t string_len;

    // --- 位置跟踪 (用于特殊情况) ---

    /**
     * @brief 用于记录多行注释或字符串字面量的起始位置。
     * @details 当遇到未闭合的注释或字符串时，可以用这个位置来报告错误。
     */
    SourceLocation block_start_loc;

} ScannerContext;


// ============================================================================
// == 函数声明
// ============================================================================

/**
 * @brief 创建并初始化一个新的 ScannerContext 实例。
 * @param filename 要解析的文件名，将存储在上下文中用于错误报告。
 * @return 指向新创建的 ScannerContext 的指针，如果失败则返回 NULL。
 */
ScannerContext* create_scanner_context(const char* filename);

/**
 * @brief 销毁一个 ScannerContext 实例，并释放其所有相关资源。
 * @param ctx 要销毁的上下文实例。
 */
void destroy_scanner_context(ScannerContext* ctx);


// ============================================================================
// == Flex/Bison 集成函数 (我们在此提供标准声明)
// ============================================================================
// 这些函数由 Flex 在生成可重入扫描器时提供。我们的驱动程序需要调用它们
// 来初始化、设置和销毁扫描器状态。将它们声明在这里，可以使代码更清晰。

/**
 * @brief 初始化一个 Flex 扫描器实例。
 * @param scanner_p 指向 yyscan_t 指针的指针，函数会在这里返回新创建的扫描器实例。
 * @return 成功时返回 0，失败时返回非 0。
 */
int yylex_init(yyscan_t* scanner_p);

/**
 * @brief 销毁一个 Flex 扫描器实例，释放其内部资源。
 * @param scanner 要销毁的扫描器实例。
 * @return 成功时返回 0。
 */
int yylex_destroy(yyscan_t scanner);

/**
 * @brief 将我们的自定义上下文（ScannerContext）与 Flex 扫描器实例关联起来。
 * @details 这是纯模式下传递状态的关键。Flex 扫描器内部会保存这个 extra 指针，
 *          之后我们可以通过 yyget_extra() 在词法规则中取回它。
 * @param extra 指向我们的 ScannerContext 实例。
 * @param scanner Flex 扫描器实例。
 */
void yyset_extra(ScannerContext* extra, yyscan_t scanner);

/**
 * @brief 获取与 Flex 扫描器关联的自定义上下文。
 * @param scanner Flex 扫描器实例。
 * @return 指向我们之前用 yyset_extra 设置的 ScannerContext 实例的指针。
 */
ScannerContext* yyget_extra(yyscan_t scanner);

/**
 * @brief 设置 Flex 扫描器的输入源。
 * @param in_str 指向一个打开的 FILE 指针，词法分析器将从此读取内容。
 * @param scanner Flex 扫描器实例。
 */
void yyset_in(FILE* in_str, yyscan_t scanner);

#endif // SCANNER_CONTEXT_H