#ifndef IR_PRINTER_H
#define IR_PRINTER_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ir_printer.h
 * @brief 声明用于将内存中的自研IR打印成文本格式的函数。
 */

// 前向声明，避免在头文件中暴露IR数据结构的完整定义，降低耦合。
typedef struct IRModule IRModule;
typedef struct IRFunction IRFunction;
typedef struct IRBasicBlock IRBasicBlock;

/**
 * @brief 将整个内存中的IR模块以文本形式打印到指定的输出流。
 * @param module 要打印的IR模块。
 * @param out 目标输出文件流，例如 `stdout` 或一个文件句柄。
 */
void print_ir_module(IRModule* module, FILE* out);

/**
 * @brief 将单个内存中的IR函数以文本形式打印到指定的输出流。
 * @param func 要打印的IR函数。
 * @param out 目标输出文件流。
 */
void print_function(IRFunction* func, FILE* out);

/**
 * @brief 将单个内存中的基本块以文本形式打印到指定的输出流。
 * @param bb 要打印的IR基本块。
 * @param out 目标输出文件流。
 */
void print_basic_block(IRBasicBlock* bb, FILE* out);

/**
 * @brief 将整个IR模块以文本形式打印到指定的文件。
 * @details
 * 这是一个便捷的封装函数，它负责处理文件的打开和关闭，
 * 内部调用 `print_ir_module` 来完成实际的打印工作。
 * @param module 要打印的IR模块。
 * @param filename 目标输出文件的路径。
 */
void print_ir_to_file(IRModule* module, const char* filename);

#ifdef __cplusplus
}
#endif

#endif // IR_PRINTER_H