#ifndef LICM_H
#define LICM_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file licm.h
 * @brief 定义循环不变量外提（Loop-Invariant Code Motion, LICM）优化遍的公共接口。
 */

/**
 * @brief 在一个函数上运行循环不变量外提（LICM）优化遍。
 *
 * @details
 * 此优化遍会查找在循环每次迭代中都计算出相同结果的指令（即“循环不变量”），
 * 并将它们从循环体中移动到循环的前置头（preheader）中。这样，这些计算
 * 在整个循环执行期间就只需要执行一次，从而减少了计算量。
 *
 * **先决条件**: 必须已经对该函数运行了 CFG、支配树和循环分析。
 * 同时，确保所有循环都有前置头是此优化能够有效执行的关键。
 *
 * @param func 要进行优化的函数。
 * @return 如果有任何代码被移出循环，则返回 true，否则返回 false。
 */
bool run_licm(IRFunction* func);

#endif // LICM_H