#ifndef IR_TRANSFORMS_SCCP_H
#define IR_TRANSFORMS_SCCP_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file sccp.h
 * @brief 定义稀疏条件常量传播（Sparse Conditional Constant Propagation, SCCP）
 *        优化遍的公共接口。
 */

/**
 * @brief 对一个给定的函数运行 SCCP 优化。
 *
 * @details
 * 此优化遍执行一种“乐观”的分析来确定 IR 中的值是否为常量。
 * 与简单的常量折叠不同，SCCP 能够跟踪控制流的变化。例如，如果一个
 * 条件分支的条件被推断为常量，那么 SCCP 就能确定只有一个分支是可达的。
 *
 * 这个过程同时完成了两件事：
 * 1.  **常量传播**：如果一个值被确定为常量，它会用这个常量值替换掉所有对该值的使用。
 * 2.  **死代码消除**：通过跟踪基本块的可达性，任何不可达的基本块及其包含的所有指令
 *     都可以被安全地移除。
 *
 * @param func 要进行优化的函数。
 * @return 如果函数被此优化遍修改过（例如，替换了常量或删除了死代码），则返回 `true`，否则返回 `false`。
 */
bool run_sccp(IRFunction* func);

#endif // IR_TRANSFORMS_SCCP_H