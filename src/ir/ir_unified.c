#include "ir/ir.h"
#include "ir/ir_generator.h"
#include "ir/ir_optimizer.h"
#include "ir/ir_printer.h"
#include <stdio.h>
#include <stdbool.h>          // for bool, false, true

/**
 * @file ir_unified.c
 * @brief 实现编译器驱动程序（driver）调用的统一 IR 接口。
 * @details
 * 本文件提供了 `ir.h` 中声明的高层函数的具体实现，
 * 它负责将IR生成、优化和代码生成等各个子模块串联起来，
 * 完成从AST到最终目标代码的完整流程。
 */

/**
 * @brief 只生成 IRModule 并返回指针，不再负责打印和销毁。
 * @param ast_ctx AST 上下文。
 * @return 生成的 IRModule*，由调用者负责后续处理和销毁。
 */
IRModule* generate_ir(ASTContext* ast_ctx) {
    if (!ast_ctx) return NULL;
    return generate_ir_module(ast_ctx);
}

/**
 * @brief 优化内存中的 IR 模块并写入文件。
 * @details
 * 这是中端优化的入口点。它接收一个已生成的 IR 模块，
 * 在其上运行优化流水线，然后将优化后的结果写回文件。
 *
 * @param module 指向待优化的、内存中的 IR 模块。
 * @param output_filename 优化后的 IR 文件的路径。
 * @return 成功返回 true，失败返回 false。
 */
bool optimize_ir(IRModule* module, const char* output_filename) {
    if (!module || !output_filename) {
        return false;
    }
    
    // 对模块进行就地（in-place）优化。
    run_optimization_pipeline(module);
    
    // 将优化后的 IR 打印到文件。
    print_ir_to_file(module, output_filename);
    
    return true;
}

// 删除 generate_riscv_assembly 的实现，只保留调用 backend_riscv.c 的实现。