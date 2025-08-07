#ifndef IR_TRANSFORMS_INLINER_H
#define IR_TRANSFORMS_INLINER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file inliner.h
 * @brief 定义函数内联（Function Inlining）优化遍的公共接口。
 */

/**
 * @brief 对一个模块中的所有函数运行函数内联优化遍。
 *
 * @details
 * 函数内联是一种重要的过程间优化（Interprocedural Optimization, IPO）。
 * 它将一个函数调用（call site）替换为被调用函数（callee）的完整函数体。
 *
 * 主要优点包括：
 * - **消除函数调用开销**：节省了参数传递、栈帧建立和销毁等成本。
 * - **创造更多优化机会**：将被调用函数体内的指令暴露给调用者（caller）的
 *   上下文，使得其他优化（如常量传播、死代码消除）能够跨越函数边界，
 *   发挥更大的作用。
 *
 * 此优化遍会根据一系列启发式规则（如函数大小、是否递归等）来决定是否
 * 对一个函数调用进行内联。
 *
 * @param func 指向模块中某个函数的指针（通常从模块的第一个函数开始）。
 *             该遍会扫描整个模块的所有函数。
 * @return 如果IR被此优化遍修改过，则返回 `true`，否则返回 `false`。
 */
bool run_inliner(IRFunction* func);

#ifdef __cplusplus
}
#endif

#endif // IR_TRANSFORMS_INLINER_H