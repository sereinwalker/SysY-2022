#ifndef IR_TRANSFORMS_IND_VAR_SIMPLIFY_H
#define IR_TRANSFORMS_IND_VAR_SIMPLIFY_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file ind_var_simplify.h
 * @brief 定义归纳变量化简（Induction Variable Simplification）优化遍的公共接口。
 */

/**
 * @brief 对一个函数运行归纳变量化简优化遍。
 *
 * @details
 * 此优化遍会识别出循环中的归纳变量。归纳变量是在每次循环迭代中以固定步长
 * 增加或减少的变量（例如循环计数器 `i`）。
 *
 * 该优化的核心是**强度削弱 (Strength Reduction)**：它会找到那些由基本归纳变量
 * 派生出来的、计算更复杂的变量（例如 `j = i * 4`），并用一个更简单、更高效的
 * 计算来替换它。具体来说，它会将循环内部昂贵的乘法运算（强度高）替换为
 * 一个简单的加法运算（强度低），从而提升生成代码的性能。
 *
 * @param func 要进行优化的函数。
 * @return 如果IR被此优化遍修改过，则返回 `true`，否则返回 `false`。
 */
bool run_ind_var_simplify(IRFunction* func);

#endif // IR_TRANSFORMS_IND_VAR_SIMPLIFY_H