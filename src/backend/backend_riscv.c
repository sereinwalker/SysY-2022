#include "backend_riscv.h"
#include "logger.h"

// Include necessary LLVM C API headers
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Types.h>
#include <stdlib.h>

/**
 * @file backend_riscv.c
 * @brief Implements the LLVM backend for generating RISC-V assembly using the LLVM C API.
 */

int generate_riscv_assembly(IRModule* module, const char* output_filename) {
    (void)module; // TODO: Use module parameter when IR to LLVM conversion is implemented
    LOG_INFO(module->log_config, LOG_CATEGORY_BACKEND, "Starting Backend phase: Generating RISC-V assembly for '%s'", output_filename);

    // For now, we'll use a temporary approach since we don't have LLVM IR file generation yet
    // In a full implementation, we would convert the IRModule to LLVM IR format first
    
    // --- Step 1: Create LLVM context and module ---
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef llvm_module = LLVMModuleCreateWithNameInContext("sysy_module", context);
    
    // TODO: Convert IRModule to LLVM IR format
    // This is a placeholder implementation
    
    // --- Step 2: Initialize LLVM Targets for RISC-V ---
    LLVMInitializeRISCVTargetInfo();
    LLVMInitializeRISCVTarget();
    LLVMInitializeRISCVTargetMC();
    LLVMInitializeRISCVAsmPrinter();
    LLVMInitializeRISCVAsmParser();

    // --- Step 3: Configure the Target ---
    const char* target_triple = "riscv64-unknown-linux-gnu";
    LLVMSetTarget(llvm_module, target_triple);

    LLVMTargetRef target;
    char* error = NULL;
    if (LLVMGetTargetFromTriple(target_triple, &target, &error)) {
        LOG_ERROR(module->log_config, LOG_CATEGORY_BACKEND, "Failed to get target from triple '%s': %s", target_triple, error);
        LLVMDisposeMessage(error);
        LLVMDisposeModule(llvm_module);
        LLVMContextDispose(context);
        return 1;
    }
    
    // --- Step 4: Create TargetMachine with specific features ---
    const char* cpu = "generic-rv64";
    const char* features = "";
    LLVMCodeGenOptLevel opt_level = LLVMCodeGenLevelDefault;
    LLVMRelocMode reloc_mode = LLVMRelocDefault;
    LLVMCodeModel code_model = LLVMCodeModelMedium;

    LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(
        target, target_triple, cpu, features, opt_level, reloc_mode, code_model
    );

    if (!target_machine) {
        LOG_ERROR(module->log_config, LOG_CATEGORY_BACKEND, "Failed to create LLVM Target Machine.");
        LLVMDisposeModule(llvm_module);
        LLVMContextDispose(context);
        return 1;
    }

    // --- Step 5: Set Module DataLayout based on the TargetMachine ---
    LLVMTargetDataRef data_layout = LLVMCreateTargetDataLayout(target_machine);
    char* data_layout_str = LLVMCopyStringRepOfTargetData(data_layout);
    LLVMSetDataLayout(llvm_module, data_layout_str);
    LLVMDisposeMessage(data_layout_str);
    LLVMDisposeTargetData(data_layout);

    // --- Step 6: Verify the module before code generation ---
    if (LLVMVerifyModule(llvm_module, LLVMReturnStatusAction, &error)) {
        LOG_ERROR(module->log_config, LOG_CATEGORY_BACKEND, "LLVM module verification failed: %s", error);
        LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(target_machine);
        LLVMDisposeModule(llvm_module);
        LLVMContextDispose(context);
        return 1;
    }

    // --- Step 7: Emit Assembly File ---
    if (LLVMTargetMachineEmitToFile(target_machine, llvm_module, (char*)output_filename, LLVMAssemblyFile, &error)) {
        LOG_ERROR(module->log_config, LOG_CATEGORY_BACKEND, "Failed to emit assembly file '%s': %s", output_filename, error);
        LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(target_machine);
        LLVMDisposeModule(llvm_module);
        LLVMContextDispose(context);
        return 1;
    }

    LOG_INFO(module->log_config, LOG_CATEGORY_BACKEND, "Successfully generated RISC-V assembly at '%s'.", output_filename);

    // --- Step 8: Clean up all LLVM resources ---
    LLVMDisposeTargetMachine(target_machine);
    LLVMDisposeModule(llvm_module);
    LLVMContextDispose(context);
    
    return 0; // Success
}