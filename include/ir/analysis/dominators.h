#ifndef DOMINATORS_H
#define DOMINATORS_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file dominators.h
 * @brief 定义支配树分析（Dominator Analysis）的公共接口。
 */

/**
 * @brief 为一个函数中的所有基本块计算支配信息。
 *
 * @details
 * 此分析遍会为每个基本块计算其直接支配者（immediate dominator, idom），
 * 并以此构建支配树（dominator tree）。它还会计算每个块的支配边界
 * （dominance frontier），这对于构建 SSA（静态单赋值）形式至关重要（例如，决定在何处插入 PHI 节点）。
 *
 * 分析结果会存储在 `IRBasicBlock` 结构体中：
 * - `idom`: 指向直接支配者的指针。
 * - `dom_children`: 在支配树中的子节点列表。
 * - `dom_frontier`: 支配边界集合。
 *
 * @param func 要分析的函数。
 */
void compute_dominators(IRFunction* func);

/**
 * @brief 检查一个基本块是否支配（dominate）另一个基本块。
 *
 * @details
 * 如果从函数入口到 `use` 块的每一条路径都必须经过 `dom` 块，
 * 那么我们称 `dom` 支配 `use`。一个块总是支配它自己。
 *
 * @param dom 潜在的支配者块。
 * @param use 要检查是否被支配的块。
 * @return 如果 `dom` 支配 `use`，返回 true，否则返回 false。
 */
bool dominates(IRBasicBlock* dom, IRBasicBlock* use);

/**
 * @brief 检查一个基本块是否严格支配（strictly dominate）另一个基本块。
 *
 * @details
 * 如果 `dom` 支配 `use`，并且 `dom` 和 `use` 不是同一个块，
 * 那么我们称 `dom` 严格支配 `use`。
 *
 * @param dom 潜在的严格支配者块。
 * @param use 要检查是否被严格支配的块。
 * @return 如果 `dom` 严格支配 `use`，返回 true，否则返回 false。
 */
bool strictly_dominates(IRBasicBlock* dom, IRBasicBlock* use);

#endif // DOMINATORS_H