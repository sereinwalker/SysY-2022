/**
 * @file ir_lifecycle.c
 * @brief 实现 IRModule 的创建和销毁。
 *
 * @details
 * 本文件负责管理顶层 IR 对象 `IRModule` 的生命周期，
 * 包括从一个专用的内存池中分配它，以及最终的释放。
 * 它清晰地分离了模块生命周期管理和模块内部结构的操作。
 */

#include "ir/ir_data_structures.h"
#include "ast.h" // 需要使用 MemoryPool 的 API (create_memory_pool, pool_strdup 等)

#include <string.h>
#include "logger.h"                 // for LogConfig

// --- 模块生命周期公共 API ---

/**
 * @brief 创建一个新的、空的 IRModule。
 *
 * @details
 * 此函数是构建整个 IR 表示的起点。它会分配一个专用的内存池，
 * 后续所有与此模块相关的 IR 组件（函数、基本块、指令等）都将
 * 在这个内存池中进行分配。这种策略显著提升了分配性能并简化了内存管理。
 *
 * @param source_filename 正在编译的源文件名，存储在模块中用于诊断和调试。
 * @param log_config 指向日志配置的指针，用于在整个IR系统中保持日志配置的一致性。
 * @return 一个指向新创建的 IRModule 的指针，如果内存分配失败则返回 NULL。
 */
IRModule* create_ir_module(const char* source_filename, LogConfig* log_config) {
    // 1. 为整个模块创建一个专用的内存池。
    MemoryPool* pool = create_memory_pool();
    if (!pool) {
        // ast.c 中的实现会在失败时退出，这里是为了代码健壮性。
        return NULL;
    }

    // 2. 从新创建的内存池中分配 IRModule 结构体自身。
    IRModule* module = (IRModule*)pool_alloc(pool, sizeof(IRModule));
    if (!module) {
        // 同样，如果内存池创建成功，这里基本不会失败。
        destroy_memory_pool(pool);
        return NULL;
    }
    
    // 3. 将模块的字段初始化为一个干净的状态。
    memset(module, 0, sizeof(IRModule));
    
    // 4. 设置模块的关键属性。
    module->pool = pool;
    module->log_config = log_config;  // 设置日志配置指针

    // 5. 使用公共 API 将源文件名复制到模块的内存池中。
    if (source_filename) {
        module->source_filename = pool_strdup(pool, source_filename);
    } else {
        module->source_filename = NULL;
    }

    // 6. 声明运行时错误处理函数
    // 注意：由于不能修改运行时库，我们将在IR生成时直接处理边界检查失败
    // 这里只声明一个简单的错误处理函数，用于边界检查失败时调用
    Type* void_type = create_void_type(pool);
    Type* int_type = create_basic_type(BASIC_INT, false, pool);
    Type* error_func_params[] = { int_type, int_type };
    Type* error_func_type = create_function_type(void_type, error_func_params, 2, false, pool);
    
    // 创建外部函数符号 - 使用现有的 putf 函数作为错误处理
    IRValue* error_func = (IRValue*)pool_alloc(pool, sizeof(IRValue));
    error_func->is_constant = false;
    error_func->type = error_func_type;
    error_func->name = pool_strdup(pool, "putf");
    // 注意：这里我们使用 putf 作为错误处理函数，因为它已经在运行时库中存在
    // 在边界检查失败时，我们将调用 putf 来输出错误信息

    return module;
}

/**
 * @brief 销毁一个 IRModule 并释放所有相关内存。
 *
 * @details
 * 得益于内存池的设计，此操作极其高效。它只需释放整个内存池，
 * 这会一次性地释放所有从该池中分配的函数、基本块、指令和其他 IR 对象。
 * 无需进行复杂的遍历来逐个释放每个对象。
 *
 * @param module 要销毁的 IRModule。此调用后，该指针将失效，不应再使用。
 *        如果 module 为 NULL，函数将不执行任何操作。
 */
void destroy_ir_module(IRModule* module) {
    if (!module) {
        return;
    }

    // 清理的核心：销毁内存池会释放所有从其中分配的内存，
    // 这也包括了 'module' 对象本身。
    destroy_memory_pool(module->pool);
    
    // 'module' 指针现在是悬垂指针，因为它指向的内存已被释放。
    // 调用者有责任不再使用它。
}