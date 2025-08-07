#ifndef BACKEND_RISCV_H
#define BACKEND_RISCV_H

#include "ir/ir_data_structures.h"
#include <stdbool.h>                // for bool

/**
 * @file backend_riscv.h
 * @brief Defines the public interface for the LLVM-based backend.
 * 
 * This module provides the interface for generating RISC-V assembly code
 * from LLVM IR using the LLVM framework. It handles target-specific
 * configuration and optimization.
 */

/**
 * @brief Initializes the LLVM backend system.
 * 
 * This function must be called before any code generation can occur.
 * It sets up the LLVM target infrastructure and registers the RISC-V target.
 * 
 * @return 0 on success, non-zero on failure.
 */
int init_llvm_backend(void);

/**
 * @brief Cleans up the LLVM backend system.
 * 
 * This function should be called when the compiler is shutting down
 * to properly clean up LLVM resources.
 */
void cleanup_llvm_backend(void);
/**
 * @brief Generates a RISC-V 64-bit assembly file from the LLVM IR.
 *
 * This function orchestrates the platform-specific parts of compilation:
 * 1. Initializes the LLVM RISC-V target.
 * 2. Creates a TargetMachine configured for 64-bit RISC-V with the 'medany' code model.
 * 3. Emits the assembly code for the given module to the specified output file.
 *
 * @param program The `IRProgram` containing the LLVM module to compile.
 * @param output_filename The path to the file where the assembly code will be written.
 * @return Returns 0 on success, and a non-zero value on failure.
 */
int generate_riscv_assembly(IRModule* module, const char* output_filename);

/**
 * @brief Generates optimized RISC-V 64-bit assembly code.
 * 
 * This function performs additional target-specific optimizations before
 * generating the final assembly code.
 * 
 * @param module The IR module to compile.
 * @param output_filename The path to the output assembly file.
 * @param optimization_level The optimization level (0-3).
 * @return 0 on success, non-zero on failure.
 */
int generate_optimized_riscv_assembly(IRModule* module, const char* output_filename, int optimization_level);

/**
 * @brief Validates that the IR module is compatible with the RISC-V target.
 * 
 * @param module The IR module to validate.
 * @return true if the module is compatible, false otherwise.
 */
bool validate_riscv_compatibility(IRModule* module);

/**
 * @brief Gets information about the target architecture.
 * 
 * @return A string describing the target architecture and features.
 */
const char* get_target_info(void);

#endif // BACKEND_RISCV_H
