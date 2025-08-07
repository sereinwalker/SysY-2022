#ifndef LOOP_ANALYSIS_H
#define LOOP_ANALYSIS_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file loop_analysis.h
 * @brief 定义用于在函数的CFG中查找和分析循环的接口。
 */

// 前向声明，因为 Loop 结构体定义在 ir_data_structures.h 中
typedef struct Loop Loop;

/**
 * @brief 查找一个函数中的所有自然循环（natural loops）。
 *
 * @details
 * 此分析基于CFG中的回边（back-edge）和支配信息来识别循环。
 * 它能够构建出循环的嵌套层级关系。
 * 分析的结果会存储在 `func->top_level_loops` 字段中，这是一个顶层循环的链表。
 * **先决条件**: 必须已经对该函数运行了CFG构建和支配树分析。
 *
 * @param func 要分析的函数。
 * @return 指向顶层循环链表头部的指针。
 */
Loop* find_loops(IRFunction* func);

/**
 * @brief 一个工具性的优化遍，确保每个循环都有一个前置头（preheader）。
 *
 * @details
 * 前置头是一个支配循环头（header）且其唯一后继是循环头的基本块。
 * 拥有前置头可以极大地简化许多循环优化（如循环不变量外提LICM），
 * 因为它提供了一个在进入循环前、只执行一次的位置来插入代码。
 *
 * @param func 要处理的函数。
 * @return 如果创建了任何前置头，则返回 true，否则返回 false。
 */
bool ensure_loop_preheaders(IRFunction* func);

#endif // LOOP_ANALYSIS_H