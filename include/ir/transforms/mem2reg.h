#ifndef MEM2REG_H
#define MEM2REG_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file mem2reg.h
 * @brief 定义内存到寄存器（Memory-to-Register）提升优化遍的公共接口。
 */

/**
 * @brief 在一个函数上运行内存到寄存器提升（Mem2Reg）优化遍。
 *
 * @details
 * 此优化遍将栈上分配的变量（由 `alloca` 指令创建）提升为 SSA 形式的虚拟寄存器，
 * 通过在控制流的汇合点插入 PHI 节点来实现。它尽可能地将值保存在寄存器中，
 * 从而消除不必要的内存加载（load）和存储（store）操作。
 *
 * 此遍的工作流程如下：
 * 1. **识别可提升的 alloca**：找出那些只被 `load` 和 `store` 指令使用的 `alloca`。
 * 2. **计算支配边界**：对每个可提升的 `alloca`，找出所有对其进行定义的块，并计算这些块的支配边界。
 * 3. **插入 PHI 节点**：在计算出的支配边界点插入 PHI 节点。
 * 4. **变量重命名**：用一个递归算法遍历支配树，将对 `alloca` 的 `load` 和 `store` 替换为对 SSA 值的直接使用。
 * 5. **清理**：移除原始的、现在已经无用的 `alloca` 指令。
 *
 * @param func 要进行优化的函数。
 * @return 如果函数被修改，则返回 true，否则返回 false。
 */
bool run_mem2reg(IRFunction* func);

#endif // MEM2REG_H