#ifndef IR_OPTIMIZER_H
#define IR_OPTIMIZER_H

#include <stdbool.h>                // for bool
#include "logger.h"                 // for LogConfig

// 包含自研IR的核心数据结构定义
#include "ir/ir_data_structures.h"

/**
 * @file ir_optimizer.h
 * @brief 定义自研IR优化器的公共接口。
 */

// --- 优化配置 ---

/**
 * @brief 优化流水线配置结构体。
 * @details
 * 用于向优化器传递参数，以启用/禁用特定的优化遍（Pass）或调整其行为。
 * 这为编译选项（如 -O1, -O2）的实现提供了基础。
 */
typedef struct {
    bool enable_mem2reg;        ///< 启用 Mem2Reg：将栈上的局部变量提升为SSA虚拟寄存器
    bool enable_cse;            ///< 启用公共子表达式消除
    bool enable_adce;           ///< 启用激进死代码消除
    bool enable_sroa;           ///< 启用标量替换聚合（将数组拆分为多个标量）
    bool enable_licm;           ///< 启用循环不变量外提
    bool enable_loop_unroll;    ///< 启用循环展开
    bool enable_sccp;           ///< 启用稀疏条件常量传播
    bool enable_tail_call_elim; ///< 启用尾调用消除
    bool enable_inst_combine;   ///< 启用指令组合
    bool enable_simplify_cfg;   ///< 启用控制流图简化
    bool enable_ind_var_simplify; ///< 启用归纳变量简化
    bool enable_inliner;        ///< 启用函数内联
    int max_iterations;         ///< 组合优化流水线的最大迭代次数，用于达到不动点
    int max_loop_unroll_count;  ///< 循环展开的最大因子
} OptimizationConfig;

/**
 * @brief 在内存中的IR模块上运行主要的优化流水线。
 *
 * @details
 * 此函数是优化阶段的总入口。它会按照预设的顺序，调度一系列分析遍和转换遍，
 * 直接对传入的 `IRModule` 数据结构进行就地修改。
 * 此函数将使用一组默认的、较为通用的优化配置。
 *
 * @param module 指向待优化的、内存中的 `IRModule` 的指针。
 */
void run_optimization_pipeline(IRModule* module);

/**
 * @brief 使用指定配置在内存中的IR模块上运行主要的优化流水线。
 *
 * @details
 * 此函数是优化阶段的总入口。它会按照预设的顺序，调度一系列分析遍和转换遍，
 * 直接对传入的 `IRModule` 数据结构进行就地修改。
 * 此函数接受一个 `OptimizationConfig` 参数，允许调用者精确控制启用的优化遍。
 *
 * @param module 指向待优化的、内存中的 `IRModule` 的指针。
 * @param config 指向优化配置的指针，如果为 NULL 则使用默认配置。
 */
void run_optimization_pipeline_with_config(IRModule* module, const OptimizationConfig* config);

/**
 * @brief 打印优化统计信息。
 * @param log_config 指向日志配置的指针，如果为 NULL 则使用默认配置。
 */
void print_optimization_stats(LogConfig* log_config);

/**
 * @brief 重置优化统计计数器。
 *
 * @details
 * 在多次运行优化或进行测试时，用于清空内部的统计数据。
 */
void reset_optimization_stats(void);

#endif // IR_OPTIMIZER_H