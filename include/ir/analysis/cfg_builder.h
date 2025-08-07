#ifndef CFG_BUILDER_H
#define CFG_BUILDER_H
#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file cfg_builder.h
 * @brief 定义控制流图（Control Flow Graph, CFG）构建的公共接口。
 */

/**
 * @brief 为一个函数构建控制流图。
 *
 * @details
 * 此分析遍通过检查每个基本块的终结符指令（terminator），
 * 来建立基本块之间的前驱（predecessor）和后继（successor）关系。
 * 它会填充每个 `IRBasicBlock` 结构中的 `successors` 和 `predecessors` 数组。
 *
 * 控制流图是许多优化和分析的基础，包括：
 * - 支配树分析 (Dominator analysis)
 * - 循环检测 (Loop detection)
 * - 死代码消除 (Dead code elimination)
 * - 控制流优化 (Control flow optimizations)
 *
 * @param func 要为其构建CFG的函数。
 */
void build_cfg(IRFunction* func);

/**
 * @brief 验证一个函数的CFG结构是否合法。
 *
 * @details
 * 此函数检查CFG是否格式良好，通过验证：
 * - 所有的终结符指令都有合法的目标基本块。
 * - 前驱和后继关系是一致的（例如，如果A是B的后继，那么B必须是A的前驱）。
 * - （可选）没有无法到达的基本块（除了那些由不可达指令如 `ret` 导致的）。
 *
 * @param func 要验证的函数。
 * @return 如果CFG有效，则返回 true，否则返回 false。
 */
bool validate_cfg(IRFunction* func);

#endif // CFG_BUILDER_H