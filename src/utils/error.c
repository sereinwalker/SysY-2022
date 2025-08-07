#include "error.h" // Ensure this header is used for declarations of ErrorContext, ErrorType, ErrorSeverity, etc.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file error.c
 * @brief 实现了错误收集和报告模块的功能。
 * @details
 * 该文件提供了在 `error.h` 中声明的错误处理API的具体实现。
 * 它管理一个 `ErrorEntry`
 * 结构体的动态数组，允许编译器在一次性报告所有错误之前，
 * 从不同阶段收集多个错误。
 */

// --- 错误严重级别映射 ---

/**
 * @brief 定义了从 ErrorType 到其默认 ErrorSeverity 的静态映射。
 * @details
 * 这个数组允许我们通过错误类型快速查找其默认的严重级别，
 * 例如，内存分配失败是致命的，而缺少返回值只是一个警告。
 */
static const ErrorSeverity ERROR_SEVERITY_MAP[] = {
    [ERROR_NONE] = ERROR_SEVERITY_INFO,
    [ERROR_MEMORY_ALLOCATION] = ERROR_SEVERITY_FATAL,
    [ERROR_LEXICAL] = ERROR_SEVERITY_ERROR,
    [ERROR_SYNTAX] = ERROR_SEVERITY_ERROR,
    [ERROR_SEMANTIC] = ERROR_SEVERITY_ERROR,
    [ERROR_DUPLICATE_SYMBOL] = ERROR_SEVERITY_ERROR,
    [ERROR_UNDEFINED_VARIABLE] = ERROR_SEVERITY_ERROR,
    [ERROR_TYPE_MISMATCH] = ERROR_SEVERITY_ERROR,
    [ERROR_INVALID_CONVERSION] = ERROR_SEVERITY_ERROR,
    [ERROR_NOT_LVALUE] = ERROR_SEVERITY_ERROR,
    [ERROR_INVALID_PARAMETER] = ERROR_SEVERITY_ERROR,
    [ERROR_MISSING_RETURN] = ERROR_SEVERITY_WARNING,
    [ERROR_MAIN_FUNCTION_MISSING] = ERROR_SEVERITY_ERROR,
    [ERROR_ARRAY_DIM_MISMATCH] = ERROR_SEVERITY_ERROR,
    [ERROR_INVALID_ARRAY_INIT] = ERROR_SEVERITY_ERROR,
    [ERROR_INVALID_ARRAY_ACCESS] = ERROR_SEVERITY_ERROR,
    [ERROR_BREAK_CONTINUE_OUTSIDE_LOOP] = ERROR_SEVERITY_ERROR,
    [ERROR_LIBRARY_FUNCTION_MISUSE] = ERROR_SEVERITY_ERROR,
    [ERROR_FORMAT_STRING_INVALID] = ERROR_SEVERITY_ERROR,
    [ERROR_IR_GENERATION] = ERROR_SEVERITY_ERROR,
    [ERROR_IR_OPTIMIZATION] = ERROR_SEVERITY_WARNING,
    [ERROR_BACKEND_GENERATION] = ERROR_SEVERITY_ERROR,
    [ERROR_RUNTIME] = ERROR_SEVERITY_ERROR,
    [ERROR_INVALID_ASSIGNMENT] = ERROR_SEVERITY_ERROR,
    [ERROR_INVALID_CONTROL_FLOW] = ERROR_SEVERITY_ERROR};

ErrorSeverity get_error_severity(ErrorType type) {
  // 检查类型是否在映射数组的有效范围内。
  if (type >= 0 &&
      type < sizeof(ERROR_SEVERITY_MAP) / sizeof(ERROR_SEVERITY_MAP[0])) {
    return ERROR_SEVERITY_MAP[type];
  }
  // 对于未知的错误类型，默认返回 ERROR。
  return ERROR_SEVERITY_ERROR;
}

const char *get_error_severity_string(ErrorSeverity severity) {
  switch (severity) {
  case ERROR_SEVERITY_INFO:
    return "info";
  case ERROR_SEVERITY_WARNING:
    return "warning";
  case ERROR_SEVERITY_ERROR:
    return "error";
  case ERROR_SEVERITY_FATAL:
    return "fatal";
  default:
    return "unknown";
  }
}

const char *get_error_type_string(ErrorType type) {
  switch (type) {
  case ERROR_NONE:
    return "none";
  case ERROR_MEMORY_ALLOCATION:
    return "memory_allocation";
  case ERROR_LEXICAL:
    return "lexical";
  case ERROR_SYNTAX:
    return "syntax";
  case ERROR_DUPLICATE_SYMBOL:
    return "duplicate_symbol";
  case ERROR_UNDEFINED_VARIABLE:
    return "undefined_variable";
  case ERROR_TYPE_MISMATCH:
    return "type_mismatch";
  case ERROR_INVALID_CONVERSION:
    return "invalid_conversion";
  case ERROR_NOT_LVALUE:
    return "not_lvalue";
  case ERROR_INVALID_PARAMETER:
    return "invalid_parameter";
  case ERROR_MISSING_RETURN:
    return "missing_return";
  case ERROR_MAIN_FUNCTION_MISSING:
    return "main_function_missing";
  case ERROR_ARRAY_DIM_MISMATCH:
    return "array_dim_mismatch";
  case ERROR_INVALID_ARRAY_INIT:
    return "invalid_array_init";
  case ERROR_INVALID_ARRAY_ACCESS:
    return "invalid_array_access";
  case ERROR_BREAK_CONTINUE_OUTSIDE_LOOP:
    return "break_continue_outside_loop";
  case ERROR_LIBRARY_FUNCTION_MISUSE:
    return "library_function_misuse";
  case ERROR_FORMAT_STRING_INVALID:
    return "format_string_invalid";
  case ERROR_IR_GENERATION:
    return "ir_generation";
  case ERROR_IR_OPTIMIZATION:
    return "ir_optimization";
  case ERROR_BACKEND_GENERATION:
    return "backend_generation";
  case ERROR_RUNTIME:
    return "runtime";
  case ERROR_INVALID_ASSIGNMENT:
    return "invalid_assignment";
  case ERROR_INVALID_CONTROL_FLOW:
    return "invalid_control_flow";
  default:
    return "unknown";
  }
}

// --- 参数验证函数 ---

bool validate_error_parameters(const ErrorContext *ctx, ErrorType type,
                               const char *msg) {
  if (!ctx) {
    fprintf(stderr, "FATAL: Error context is NULL\n");
    return false;
  }

  if (type < 0 ||
      type >= sizeof(ERROR_SEVERITY_MAP) / sizeof(ERROR_SEVERITY_MAP[0])) {
    fprintf(stderr, "FATAL: Invalid error type: %d\n", type);
    return false;
  }

  if (!msg) {
    fprintf(stderr, "FATAL: Error message is NULL\n");
    return false;
  }

  if (strlen(msg) == 0) {
    fprintf(stderr, "FATAL: Error message is empty\n");
    return false;
  }

  return true;
}

bool validate_source_location(const SourceLocation *loc) {
  if (!loc) {
    return false;
  }

  // 基本验证：行号和列号应为正数。
  if (loc->first_line <= 0 || loc->first_column <= 0) {
    return false;
  }

  // 结束位置不应在开始位置之前。
  if (loc->last_line < loc->first_line) {
    return false;
  }

  if (loc->last_line == loc->first_line &&
      loc->last_column < loc->first_column) {
    return false;
  }

  return true;
}

// --- 增强的错误上下文 API 实现 ---

// 注释说明 ErrorContext 典型用法为值类型，推荐自动管理
bool init_error_context(ErrorContext *ctx, size_t initial_capacity) {
  if (ctx == NULL) {
    fprintf(stderr, "FATAL: Cannot initialize a NULL ErrorContext.\n");
    return false;
  }

  if (initial_capacity == 0) {
    initial_capacity = 16; // 如果提供0，则使用默认容量。
  }

  // 为错误数组分配初始内存。
  ctx->errors = malloc(initial_capacity * sizeof(ErrorEntry));
  // 为错误上下文分配内存失败是一个致命的、不可恢复的错误。
  if (ctx->errors == NULL) {
    perror("FATAL: Failed to allocate memory for the error context");
    return false;
  }

  // 初始化计数器和标志。
  ctx->count = 0;
  ctx->capacity = initial_capacity;
  ctx->has_fatal_errors = false;

  return true;
}

bool add_error(ErrorContext *ctx, ErrorType type, const char *msg,
               SourceLocation loc) {
  if (!validate_error_parameters(ctx, type, msg)) {
    return false;
  }

  // 获取此错误类型的默认严重级别。
  ErrorSeverity severity = get_error_severity(type);
  // 调用更底层的函数来添加错误。
  return add_error_with_severity(ctx, type, severity, msg, loc);
}

bool add_error_with_severity(ErrorContext *ctx, ErrorType type,
                             ErrorSeverity severity, const char *msg,
                             SourceLocation loc) {
  if (!validate_error_parameters(ctx, type, msg)) {
    return false;
  }

  if (severity < ERROR_SEVERITY_INFO || severity > ERROR_SEVERITY_FATAL) {
    fprintf(stderr, "FATAL: Invalid error severity: %d\n", severity);
    return false;
  }

  if (!validate_source_location(&loc)) {
    // 如果提供的位置无效，则使用一个安全（但可能不准确）的默认位置。
    loc = (SourceLocation){1, 1, 1, 1};
  }

  // 检查动态数组是否已满；如果是，则扩展其容量。
  if (ctx->count >= ctx->capacity) {
    // 将容量加倍，或者如果容量为0，则从16开始。
    size_t new_capacity = (ctx->capacity == 0) ? 16 : ctx->capacity * 2;
    ErrorEntry *new_errors =
        realloc(ctx->errors, new_capacity * sizeof(ErrorEntry));

    // 重新分配错误列表失败是一个致命的、不可恢复的错误。
    // 如果编译器无法记录新的错误，它就无法继续。
    if (!new_errors) {
      fprintf(stderr,
              "Fatal Error: Failed to reallocate memory for the error list.\n");
      // 在退出前，为保持整洁而释放旧的（较小的）列表。
      free(ctx->errors);
      exit(EXIT_FAILURE);
    }

    ctx->errors = new_errors;
    ctx->capacity = new_capacity;
  }

  // 获取指向下一个可用的 `ErrorEntry` 槽的指针。
  ErrorEntry *new_entry = &ctx->errors[ctx->count];

  // 填充错误信息。
  new_entry->type = type;
  new_entry->severity = severity;
  new_entry->loc = loc;

  // 使用 snprintf 安全地复制错误消息，以防止缓冲区溢出。
  snprintf(new_entry->message, sizeof(new_entry->message), "%s", msg);
  // 确保字符串以空字符结尾，即使原始消息被截断。
  new_entry->message[sizeof(new_entry->message) - 1] = '\0';

  // 增加已记录错误的计数。
  ctx->count++;

  // 如果这是一个致命错误，则更新致命错误标志。
  if (severity == ERROR_SEVERITY_FATAL) {
    ctx->has_fatal_errors = true;
  }

  return true;
}

void free_error_context(ErrorContext *ctx) {
  if (ctx != NULL) {
    // 释放存储错误条目的主数组。
    // `free()` 可以安全地处理 NULL 指针。
    free(ctx->errors);

    // 将指针和计数器重置为安全的、干净的状态，以防止悬挂指针。
    ctx->errors = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    ctx->has_fatal_errors = false;
  }
}

bool has_fatal_errors(const ErrorContext *ctx) {
  return ctx ? ctx->has_fatal_errors : false;
}

size_t get_error_count_by_severity(const ErrorContext *ctx,
                                   ErrorSeverity severity) {
  if (!ctx)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < ctx->count; ++i) {
    if (ctx->errors[i].severity == severity) {
      count++;
    }
  }
  return count;
}

size_t get_total_error_count(const ErrorContext *ctx) {
  if (!ctx)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < ctx->count; ++i) {
    // 只计算警告和错误，不包括信息性消息。
    if (ctx->errors[i].severity >= ERROR_SEVERITY_WARNING) {
      count++;
    }
  }
  return count;
}