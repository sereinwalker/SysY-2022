#ifndef IR_GENERATOR_H
#define IR_GENERATOR_H

// 包含自研IR的核心数据结构定义
#include "ir/ir_data_structures.h"
#include "ast.h"                    // for ASTContext

// 前向声明，以避免与 ast.h 的循环依赖
typedef struct ASTContext ASTContext;

/**
 * @file ir_generator.h
 * @brief 定义从AST到自研IR的生成器接口。
 */

/**
 * @brief 从抽象语法树（AST）生成一个完整的、内存中的IR模块。
 *
 * @details
 * 这是IR生成阶段的核心驱动函数。它会遍历整个AST，
 * 并根据AST节点的语义，在内存中逐步构建起一个完整的 `IRModule`，
 * 包括所有的全局变量、函数、函数内的基本块以及基本块内的指令。
 *
 * @param ast_ctx 包含待翻译AST树根的AST上下文，其中包含了符号表等重要信息。
 * @return 一个指向新创建并已填充内容的 `IRModule` 的指针。
 *         此模块的所有权转移给调用者，调用者必须在后续使用
 *         `destroy_ir_module()` 来释放其内存。
 */
IRModule* generate_ir_module(ASTContext* ast_ctx);

#endif // IR_GENERATOR_H