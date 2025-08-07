#ifndef IR_TRANSFORMS_LOOP_UNROLL_H
#define IR_TRANSFORMS_LOOP_UNROLL_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file loop_unroll.h
 * @brief 定义循环展开（Loop Unroll）优化遍的公共接口。
 */

/**
 * @brief 对一个函数中的所有循环执行循环展开优化。
 *
 * @details
 * 此优化遍会识别出程序中符合特定模式的简单循环结构（类似于由 `while` 循环
 * 生成的、带有归纳变量的计数循环），并将其循环体展开一个固定的次数。
 * 这样做的目的是为了减少循环本身的开销（如跳转和条件判断），并为后续的其他
 * 优化（如指令调度）暴露更多的指令级并行性。
 *
 * 此函数只会展开满足严格条件的循环，包括：
 * - 循环必须具有简单的控制流结构（例如，单个前继、单个回边）。
 * - 循环必须有一个规范化的归纳变量（例如，`i = i + 常量`）。
 * - 循环的总迭代次数必须在编译时可知。
 *
 * @param func 要进行优化的函数。
 * @return 如果函数中的任何一个循环被成功展开，则返回 `true`，否则返回 `false`。
 */
bool run_loop_unroll(IRFunction* func);

#endif // IR_TRANSFORMS_LOOP_UNROLL_H