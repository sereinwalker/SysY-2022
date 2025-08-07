// --- START OF FILE ir/transforms/cse.h ---

#ifndef CSE_H
#define CSE_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file cse.h
 * @brief Defines the public interface for the Common Subexpression Elimination pass.
 */

/**
 * @brief Runs the Global Common Subexpression Elimination (GCSE) pass on a function.
 *
 * This pass identifies and eliminates redundant computations that are common
 * across different basic blocks. It uses a dominator tree traversal to ensure
 * that a subexpression is available at all points where it is being replaced.
 *
 * @param func The function to be optimized.
 * @return true if the function was modified, false otherwise.
 */
bool run_cse(IRFunction* func);

#endif // CSE_H