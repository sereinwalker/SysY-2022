#ifndef IR_H
#define IR_H

// 包含自研IR的核心数据结构定义
#include "ir/ir_data_structures.h"

/**
 * @file ir.h
 * @brief 自研中间表示（IR）阶段的统一高层接口。
 * 
 * @details
 * 此头文件为编译器中端（Middle-end）和后端（Back-end）的入口，
 * 提供了一系列高级函数来驱动整个IR的处理流程，从生成、优化到最终的目标代码生成。
 * 它封装了内部复杂的IR操作，为编译器驱动程序（driver）提供了简洁的调用方式。
 */

/**
 * @brief 对内存中的IR模块应用优化，并写回文件。
 * 
 * @details
 * 此函数驱动IR优化流水线，对传入的IR模块进行一系列的就地修改。
 * 优化完成后，将修改后的IR模块打印到指定的输出文件。
 * 
 * @param module 指向待优化的、内存中的IR模块。
 * @param output_filename 要将优化后的IR写入的文件路径。
 * @return 成功时返回 true，失败时返回 false。
 */
bool optimize_ir(IRModule* module, const char* output_filename);

/**
 * @brief 从内存中的IR模块生成RISC-V汇编代码。
 * 
 * @details
 * 这是连接中端和后端的桥梁。它驱动目标代码生成器，
 * 将内存中的IR翻译成目标体系结构（RISC-V）的汇编代码。
 * 
 * @param module 指向包含IR的模块。
 * @param output_filename 要将生成的汇编代码写入的文件路径。
 * @return 成功时返回 0，失败时返回非零值。
 */
int generate_riscv_assembly(IRModule* module, const char* output_filename);

/**
 * @brief 销毁一个IR模块，释放其占用的所有内存。
 * @details
 * 由于IR模块及其内容（函数、基本块、指令）都是在内存中动态创建的，
 * 此函数负责递归地释放所有相关资源，防止内存泄漏。
 * @param module 要销毁的IR模块。
 */
void destroy_ir_module(IRModule* module);

// 只生成 IRModule 并返回指针，由调用者负责后续处理和销毁
IRModule* generate_ir(ASTContext* ast_ctx);

#endif // IR_H