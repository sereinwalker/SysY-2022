#ifndef IR_TRANSFORMS_TAIL_CALL_ELIM_H
#define IR_TRANSFORMS_TAIL_CALL_ELIM_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file tail_call_elim.h
 * @brief 定义尾调用消除（Tail Call Elimination, TCE）优化遍的公共接口。
 */

/**
 * @brief 对一个函数运行尾调用消除优化。
 *
 * @details
 * 尾调用是指一个函数返回前执行的最后一个操作是函数调用。对于**直接的尾递归调用**
 * （即一个函数在尾部调用自身），可以将其优化为一个循环，从而避免函数调用栈的
 * 无限增长，将递归转换为迭代。
 *
 * 此优化遍会识别出形如 `return a(...)` 的直接尾递归调用，并执行以下转换：
 * 1.  用新参数（来自 `call` 指令的实参）更新函数入口 PHI 节点的值。
 * 2.  移除 `call` 和 `ret` 指令。
 * 3.  插入一个无条件跳转，直接跳回函数的入口块。
 *
 * @param func 要进行优化的函数。
 * @return 如果函数被此优化遍修改过，则返回 `true`，否则返回 `false`。
 */
bool run_tail_call_elim(IRFunction* func);

#endif // IR_TRANSFORMS_TAIL_CALL_ELIM_H