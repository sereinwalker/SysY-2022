#ifndef ADCE_H
#define ADCE_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file adce.h
 * @brief 定义激进死代码消除（Aggressive Dead Code Elimination）优化遍的公共接口。
 */

/**
 * @brief 在一个函数上运行激进死代码消除（ADCE）优化遍。
 *
 * @details
 * 此优化遍识别并移除“死”指令。一条指令被认为是死的，如果它的计算结果
 * 从未被使用，并且它自身不具有任何副作用（如内存写入、函数调用、控制流改变等）。
 * 与简单的DCE不同，ADCE能够处理跨越控制流边界的死代码。
 *
 * 它的工作原理是，首先假设所有代码都是死的，然后从已知“活”的指令
 * （那些有副作用的指令）开始，在数据流和控制流图上反向追溯，
 * 将所有对活指令有贡献的指令也标记为活的。最后，所有未被标记的指令
 * 都将被移除。
 *
 * @param func 要进行优化的函数。
 * @return 如果函数被修改，则返回 true，否则返回 false。
 */
bool run_adce(IRFunction* func);

#endif // ADCE_H