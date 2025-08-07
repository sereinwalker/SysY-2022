/**
 * @file ir_optimizer.c
 * @brief 优化器的主要驱动程序。管理并运行一个综合的优化流水线。
 * @details
 * 本文件实现了编译器的中端优化阶段。它采用一个多阶段、迭代式的
 * 优化流水线，协同调用多个独立的优化遍（Passes），以逐步地、系统地
 * 提升 IR 的质量。
 *
 * 流水线设计思想：
 * 1.  **分析先行**: 在所有变换之前，运行必要的分析遍（CFG, Dominators）。
 * 2.  **规范化**: 将 IR 转换为对优化更友好的形式，核心是 SROA + Mem2Reg，
 *     构建严格的 SSA 形式。
 * 3.  **核心迭代优化**: 在一个不动点迭代循环中，反复运行一系列相互促进的
 *     优化遍（如 InstCombine, SCCP, CSE, ADCE），直到 IR 不再发生变化。
 * 4.  **循环优化**: 在标量优化稳定后，集中处理循环相关的优化（LICM, IndVar,
 * Unroll）。
 * 5.  **过程间优化 (IPO)**: 在函数内优化之后，进行跨函数的优化（Inliner,
 * TCE）。
 * 6.  **持续清理**: 在关键阶段后运行 CFG 简化，以清理冗余代码，为后续优化
 *     提供更干净的输入。
 */
#include "ir/ir_optimizer.h"
#include "ir/ir_data_structures.h"
#include "logger.h"
#include <string.h>

// --- 包含所有分析遍和优化遍的头文件 ---
#include "ir/analysis/cfg_builder.h"
#include "ir/analysis/dominators.h"
#include "ir/analysis/loop_analysis.h"
#include "ir/transforms/adce.h"
#include "ir/transforms/cse.h"
#include "ir/transforms/ind_var_simplify.h"
#include "ir/transforms/inliner.h"
#include "ir/transforms/inst_combine.h"
#include "ir/transforms/licm.h"
#include "ir/transforms/loop_unroll.h"
#include "ir/transforms/mem2reg.h"
#include "ir/transforms/sccp.h"
#include "ir/transforms/simplify_cfg.h"
#include "ir/transforms/sroa.h"
#include "ir/transforms/tail_call_elim.h"

// --- 函数级优化流水线的前向声明 ---
static void optimize_function(IRFunction *func,
                              const OptimizationConfig *config);

// --- 默认优化配置 (-O1 级别) ---
static const OptimizationConfig DEFAULT_CONFIG = {
    .enable_mem2reg = true,
    .enable_cse = true,
    .enable_adce = true,
    .enable_sroa = true,
    .enable_licm = true,
    .enable_loop_unroll = false, // 循环展开会显著增加代码大小，默认关闭
    .enable_sccp = true,
    .enable_tail_call_elim = true,
    .enable_inst_combine = true,
    .enable_simplify_cfg = true,
    .enable_ind_var_simplify = true,
    .enable_inliner = true,
    .max_iterations = 10,      // 迭代优化的最大次数
    .max_loop_unroll_count = 4 // 循环展开因子
};

// --- 主优化流水线 ---

/**
 * @brief 使用默认配置运行优化流水线。
 */
void run_optimization_pipeline(IRModule *module) {
  run_optimization_pipeline_with_config(module, &DEFAULT_CONFIG);
}

/**
 * @brief 使用指定配置运行优化流水线。
 * @details
 * 这是一个模块级的 Pass Manager。它首先对模块中的每个函数运行函数级优化，
 * 然后运行模块级的过程间优化（IPO）。
 */
void run_optimization_pipeline_with_config(IRModule *module,
                                           const OptimizationConfig *config) {
  if (!module || !module->log_config) {
    // 如果没有模块或日志配置，创建一个默认的
    LogConfig default_log_config;
    logger_config_init_default(&default_log_config);
    LOG_INFO(&default_log_config, LOG_CATEGORY_IR_GEN,
             "Starting optimization pipeline...");
    return;
  }

  LOG_INFO(module->log_config, LOG_CATEGORY_IR_GEN,
           "Starting optimization pipeline...");
  if (!config)
    config = &DEFAULT_CONFIG;

  // --- 阶段 1: 迭代的函数内优化 ---
  for (IRFunction *func = module->functions; func; func = func->next) {
    if (!func->entry)
      continue; // 跳过外部函数声明
    LOG_DEBUG(module->log_config, LOG_CATEGORY_IR_GEN,
              "Optimizing function @%s", func->name);
    optimize_function(func, config);
  }

  // --- 阶段 2: 过程间优化 (IPO) ---
  // IPO 可能会改变函数，甚至删除函数，所以在一个独立的循环中进行
  if (config->enable_inliner) {
    if (run_inliner(module->functions)) {
      // 内联后，需要对被修改过的函数再次运行优化
      for (IRFunction *func = module->functions; func; func = func->next) {
        if (!func->entry)
          continue;
        optimize_function(func, config);
      }
    }
  }

  if (config->enable_tail_call_elim) {
    for (IRFunction *func = module->functions; func; func = func->next) {
      if (!func->entry)
        continue;
      run_tail_call_elim(func);
    }
  }

  LOG_INFO(module->log_config, LOG_CATEGORY_IR_GEN,
           "Optimization pipeline completed.");
}

/**
 * @brief 对单个函数执行迭代式优化。
 * @details
 * 这是一个不动点迭代算法，它反复运行一系列优化遍，直到没有更多改变发生。
 *
 * 优化Pass执行顺序和依赖关系：
 *
 * 阶段1: 初始分析和预处理
 *   1. build_cfg()           - 构建控制流图（必须最先执行）
 *   2. compute_dominators()  - 计算支配关系（依赖CFG）
 *   3. run_sroa()           - 标量替换聚合（可选）
 *   4. run_mem2reg()        - 内存到寄存器提升（依赖支配信息）
 *
 * 阶段2: 核心标量优化迭代循环
 *   5. run_inst_combine()   - 指令合并（最先执行，为其他优化创造机会）
 *   6. run_sccp()          - 稀疏条件常量传播（依赖指令合并的结果）
 *   7. run_cse()           - 公共子表达式消除（依赖常量传播的结果）
 *   8. run_adce()          - 攻击性死代码消除（清理优化）
 *   9. run_simplify_cfg()  - CFG简化（清理优化，可能影响CFG结构）
 *   10. 重新计算CFG和支配信息（如果有改变）
 *
 * 阶段3: 循环优化（在标量优化稳定后进行）
 *   11. find_loops()         - 循环发现（依赖CFG和支配信息）
 *   12. run_licm()          - 循环不变量外提（依赖循环信息）
 *   13. run_ind_var_simplify() - 归纳变量简化（依赖循环信息）
 *   14. run_loop_unroll()   - 循环展开（可选，依赖循环信息）
 *   15. 最后一轮清理（inst_combine + adce + simplify_cfg）
 *
 * 关键依赖关系：
 * - CFG必须在所有优化之前构建
 * - 支配信息依赖CFG，用于mem2reg和循环优化
 * - mem2reg必须在标量优化之前执行
 * - 循环优化必须在标量优化稳定后执行
 * - CFG简化后需要重新计算分析信息
 */
static void optimize_function(IRFunction *func,
                              const OptimizationConfig *config) {
  if (!func || !func->module || !func->module->log_config) {
    // 如果没有函数、模块或日志配置，创建一个默认的
    LogConfig default_log_config;
    logger_config_init_default(&default_log_config);
    LOG_WARN(&default_log_config, LOG_CATEGORY_IR_GEN,
             "Cannot optimize function: missing context");
    return;
  }

  // --- 初始分析 ---
  build_cfg(func);
  compute_dominators(func);

  // --- 第一次清理和规范化 ---
  if (config->enable_sroa) {
    run_sroa(func);
  }
  if (config->enable_mem2reg) {
    run_mem2reg(func);
  }

  // --- 核心优化迭代循环 ---
  int iteration = 0;
  bool changed_in_iteration;
  do {
    changed_in_iteration = false;

    // 核心标量优化
    if (config->enable_inst_combine) {
      changed_in_iteration |= run_inst_combine(func);
    }
    if (config->enable_sccp) {
      changed_in_iteration |= run_sccp(func);
    }
    if (config->enable_cse) {
      changed_in_iteration |= run_cse(func);
    }

    // 清理遍
    if (config->enable_adce) {
      changed_in_iteration |= run_adce(func);
    }
    if (config->enable_simplify_cfg) {
      changed_in_iteration |= run_simplify_cfg(func);
    }

    // 如果CFG被简化，后续优化的效果会更好，所以重新计算分析
    if (changed_in_iteration) {
      build_cfg(func);
      compute_dominators(func);
    }
    iteration++;
  } while (changed_in_iteration && iteration < config->max_iterations);

  // --- 循环优化 (在标量优化稳定后进行) ---
  find_loops(func);
  if (func->top_level_loops) {
    if (config->enable_licm) {
      run_licm(func);
    }
    if (config->enable_ind_var_simplify) {
      run_ind_var_simplify(func);
    }
    if (config->enable_loop_unroll) {
      run_loop_unroll(func);
    }

    // 循环优化后可能产生大量冗余，进行最后一轮清理
    if (run_inst_combine(func) || run_adce(func) || run_simplify_cfg(func)) {
      // 如果清理有效，可以考虑再进行一轮核心优化，但通常一轮就足够
    }
  }

  if (iteration >= config->max_iterations) {
    LOG_WARN(func->module->log_config, LOG_CATEGORY_IR_GEN,
             "Function @%s reached max optimization iterations (%d)",
             func->name, config->max_iterations);
  }
  LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN,
            "Function @%s optimized in %d iterations", func->name, iteration);
}

// --- 优化统计 (此处省略具体实现，与之前版本相同) ---

typedef struct {
  int total_instructions_removed;
  int total_cse_eliminations;
  int total_adce_eliminations;
  int total_functions_optimized;
} OptimizationStats;

static OptimizationStats global_stats = {0};

void print_optimization_stats(LogConfig *log_config) {
  if (!log_config) {
    // 如果没有提供日志配置，创建一个默认的
    static LogConfig default_log_config;
    static bool default_initialized = false;
    if (!default_initialized) {
      logger_config_init_default(&default_log_config);
      default_initialized = true;
    }
    log_config = &default_log_config;
  }

  LOG_INFO(log_config, LOG_CATEGORY_IR_GEN, "Optimization Statistics:");
  LOG_INFO(log_config, LOG_CATEGORY_IR_GEN, "  Total instructions removed: %d",
           global_stats.total_instructions_removed);
  LOG_INFO(log_config, LOG_CATEGORY_IR_GEN, "  Total CSE eliminations: %d",
           global_stats.total_cse_eliminations);
  LOG_INFO(log_config, LOG_CATEGORY_IR_GEN, "  Total ADCE eliminations: %d",
           global_stats.total_adce_eliminations);
  LOG_INFO(log_config, LOG_CATEGORY_IR_GEN, "  Total functions optimized: %d",
           global_stats.total_functions_optimized);
}

void reset_optimization_stats(void) {
  memset(&global_stats, 0, sizeof(global_stats));
}