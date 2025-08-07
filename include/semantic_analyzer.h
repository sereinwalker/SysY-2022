#ifndef SEMANTIC_ANALYZER_H
#define SEMANTIC_ANALYZER_H

#include "ast.h"

/**
 * @file semantic_analyzer.h
 * @brief 定义语义分析阶段的公共接口。
 */

/**
 * @brief 对整个抽象语法树（AST）执行语义分析。
 * 
 * @details
 * 这是语义分析阶段的主要驱动函数。它对AST进行两次主要的遍历：
 * 1.  **符号表构建**: 遍历AST，为所有作用域构建嵌套的符号表，并用变量、常量和
 *     函数的声明来填充它们。此过程会检查在同一作用域内的重定义错误。
 * 2.  **语义检查**: 再次遍历AST，执行所有类型检查，验证符号的正确使用（例如，
 *     函数被调用，变量在使用前已定义），检查控制流（例如，非void函数有返回值），
 *     并执行常量折叠。
 * 
 * 此函数会就地修改AST：
 * - 将符号表作用域附加到相应的节点（FuncDecl, CompoundStmt）。
 * - 将解析后的符号指针附加到标识符和声明节点。
 * - 为所有表达式节点标注其求值后的类型。
 * - 将发现的所有语义错误填充到错误上下文中。
 * 
 * @param ctx 包含待分析AST根节点的AST上下文。
 */
void perform_semantic_analysis(ASTContext* ctx);

#endif // SEMANTIC_ANALYZER_H