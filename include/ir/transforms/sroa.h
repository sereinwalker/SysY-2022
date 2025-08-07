#ifndef SROA_H
#define SROA_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file sroa.h
 * @brief 定义聚合的标量替换（Scalar Replacement of Aggregates, SROA）优化遍的公共接口。
 */

/**
 * @brief 在一个函数上运行聚合的标量替换（SROA）优化遍。
 *
 * @details
 * 此优化遍尝试将为聚合类型（当前主要指数组）分配栈空间的 `alloca` 指令，
 * 分解为为该聚合体的每个标量元素单独进行 `alloca`。
 * 这样做可以将聚合体的各个元素暴露给后续的优化，特别是 `mem2reg`，
 * 从而使得数组元素也能被提升到 SSA 寄存器中，消除不必要的内存访问。
 *
 * @param func 要进行优化的函数。
 * @return 如果函数被修改，则返回 true，否则返回 false。
 */
bool run_sroa(IRFunction* func);

#endif // SROA_H