#ifndef IR_TRANSFORMS_SIMPLIFY_CFG_H
#define IR_TRANSFORMS_SIMPLIFY_CFG_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file simplify_cfg.h
 * @brief 定义控制流图（CFG）简化优化遍的公共接口。
 */

/**
 * @brief 对一个函数运行一系列的控制流图（CFG）简化变换，直到达到不动点。
 *
 * @details
 * 此优化遍是许多其他优化的基础和后续清理步骤。它会迭代地应用多种技术来
 * 清理和简化函数的控制流图，使得IR更规整，也为其他优化创造更多机会。
 *
 * 它包含以下子优化过程：
 * - **常量分支折叠 (Constant Branch Folding)**: 将基于常量的条件跳转（`br i1 true, ...`）
 *   转换为无条件跳转。
 * - **不可达块消除 (Unreachable Block Elimination)**: 移除从函数入口块无法访问到的基本块。
 * - **基本块合并 (Block Merging)**: 如果一个块B只有一个前驱A，且A只有一个后继B，
 *   则将B合并到A的末尾。
 * - **跳转线程化 (Jump Threading)**: 绕过那些只包含一个无条件跳转的“跳板”块。
 *
 * 此函数会持续运行这些子优化，直到在一整轮迭代中CFG不再发生任何变化为止。
 *
 * @param func 要被简化其CFG的函数。
 * @return 如果CFG发生了任何改变，则返回 `true`，否则返回 `false`。
 */
bool run_simplify_cfg(IRFunction* func);

#endif // IR_TRANSFORMS_SIMPLIFY_CFG_H