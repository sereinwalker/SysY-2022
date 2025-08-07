#ifndef INST_COMBINE_H
#define INST_COMBINE_H

#include <stdbool.h>
#include "ir/ir_data_structures.h"

/**
 * @file inst_combine.h
 * @brief 定义指令合并（Instruction Combining）优化遍的公共接口。
 */

/**
 * @brief 对一个函数执行基本的指令合并和简化。
 *
 * @details
 * 此优化遍会遍历函数中的所有指令，并尝试将它们替换为更简单或更规范的形式。
 * 这是一种“窥孔优化”（Peephole Optimization）的推广，它能处理多种模式，包括：
 * - **常量折叠 (Constant Folding)**: 在编译时计算常量表达式，例如将 `add 2, 3` 替换为 `5`。
 * - **代数化简 (Algebraic Simplification)**: 应用代数恒等式，例如将 `x + 0` 替换为 `x`，
 *   `x * 1` 替换为 `x`，`x - x` 替换为 `0`。
 * - **强度削减 (Strength Reduction)**: 将代价高的运算替换为代价低的运算，例如将
 *   `x * 2` 替换为 `x << 1`。
 *
 * 此优化遍被设计为可以反复运行，因为一次简化可能会为另一次简化创造新的机会。
 *
 * @param func 指向待变换的 IRFunction 的指针。
 * @return 如果任何指令被改变或移除，则返回 `true`，否则返回 `false`。
 */
bool run_inst_combine(IRFunction* func);

#endif // INST_COMBINE_H