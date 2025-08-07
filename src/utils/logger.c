#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @file logger.c
 * @brief 实现了用于编译器的简单、灵活的日志记录工具。
 */

// --- 颜色支持实现 (使用 \x1b) ---

// ANSI颜色转义序列
static const char *ansi_color_codes[] = {
    "\x1b[0m",    // LOG_COLOR_RESET
    "\x1b[31m",   // LOG_COLOR_RED
    "\x1b[32m",   // LOG_COLOR_GREEN
    "\x1b[33m",   // LOG_COLOR_YELLOW
    "\x1b[34m",   // LOG_COLOR_BLUE
    "\x1b[35m",   // LOG_COLOR_MAGENTA
    "\x1b[36m",   // LOG_COLOR_CYAN
    "\x1b[37m",   // LOG_COLOR_WHITE
    "\x1b[1;31m", // LOG_COLOR_BRIGHT_RED
    "\x1b[1;32m", // LOG_COLOR_BRIGHT_GREEN
    "\x1b[1;33m", // LOG_COLOR_BRIGHT_YELLOW
    "\x1b[1;34m", // LOG_COLOR_BRIGHT_BLUE
    "\x1b[1;35m", // LOG_COLOR_BRIGHT_MAGENTA
    "\x1b[1;36m", // LOG_COLOR_BRIGHT_CYAN
    "\x1b[1;37m"  // LOG_COLOR_BRIGHT_WHITE
};

// 检查当前终端是否支持颜色输出
bool is_color_supported() {
#ifdef FORCE_COLOR_OUTPUT
  return true;
#endif
  if (!isatty(fileno(stderr))) {
    return false;
  }
  const char *term = getenv("TERM");
  if (!term) {
    return false;
  }
  return (strstr(term, "xterm") != NULL || strstr(term, "linux") != NULL ||
          strstr(term, "vt100") != NULL || strstr(term, "color") != NULL);
}

// 获取ANSI颜色转义序列
const char *get_ansi_color_code(LogColor color) {
  if (color >= 0 &&
      color < sizeof(ansi_color_codes) / sizeof(ansi_color_codes[0])) {
    return ansi_color_codes[color];
  }
  return ansi_color_codes[LOG_COLOR_RESET];
}

// 获取日志级别对应的颜色
LogColor get_log_level_color(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_ERROR:
    return LOG_COLOR_BRIGHT_RED;
  case LOG_LEVEL_WARNING:
    return LOG_COLOR_BRIGHT_YELLOW;
  case LOG_LEVEL_INFO:
    return LOG_COLOR_BRIGHT_GREEN;
  case LOG_LEVEL_DEBUG:
    return LOG_COLOR_BRIGHT_BLUE;
  case LOG_LEVEL_TRACE:
    return LOG_COLOR_BRIGHT_MAGENTA;
  default:
    return LOG_COLOR_WHITE;
  }
}

// 获取日志类别对应的颜色
LogColor get_log_category_color(LogCategory category) {
  switch (category) {
  case LOG_CATEGORY_GENERAL:
    return LOG_COLOR_WHITE;
  case LOG_CATEGORY_LEXER:
    return LOG_COLOR_CYAN;
  case LOG_CATEGORY_PARSER:
    return LOG_COLOR_BLUE;
  case LOG_CATEGORY_SEMANTIC:
    return LOG_COLOR_GREEN;
  case LOG_CATEGORY_IR_GEN:
    return LOG_COLOR_MAGENTA;
  case LOG_CATEGORY_IR_OPT:
    return LOG_COLOR_BRIGHT_MAGENTA;
  case LOG_CATEGORY_BACKEND:
    return LOG_COLOR_YELLOW;
  case LOG_CATEGORY_MEMORY:
    return LOG_COLOR_BRIGHT_CYAN;
  case LOG_CATEGORY_PERFORMANCE:
    return LOG_COLOR_BRIGHT_BLUE;
  case LOG_CATEGORY_SECURITY:
    return LOG_COLOR_BRIGHT_RED;
  default:
    return LOG_COLOR_WHITE;
  }
}

// 设置颜色输出
void set_log_color(const LogConfig *config, LogColor color, FILE *stream) {
  if (!config || !config->enable_colors || !is_color_supported()) {
    return;
  }
  fprintf(stream, "%s", get_ansi_color_code(color));
}

// 重置颜色输出
void reset_log_color(const LogConfig *config, FILE *stream) {
  if (!config || !config->enable_colors || !is_color_supported()) {
    return;
  }
  fprintf(stream, "%s", get_ansi_color_code(LOG_COLOR_RESET));
}

// --- 新的初始化函数 ---

void logger_config_init_default(LogConfig *config) {
  if (!config)
    return;
  config->level = LOG_LEVEL_INFO;
  config->enable_timestamps = true;
  config->enable_categories = true;
  config->enable_file_line = true;
  config->enable_colors = is_color_supported();
  config->enabled_category_count = 0;
  config->categories_explicitly_set = false;
}

// 检查类别是否启用的辅助函数
static bool logger_is_category_enabled(const LogConfig *config,
                                       LogCategory category) {
  if (!config->categories_explicitly_set) {
    return true;
  }
  for (int i = 0; i < config->enabled_category_count; ++i) {
    if (config->enabled_categories[i] == category) {
      return true;
    }
  }
  return false;
}

// --- 工具函数 ---

const char *get_log_level_string(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_NONE:
    return "NONE";
  case LOG_LEVEL_ERROR:
    return "ERROR";
  case LOG_LEVEL_WARNING:
    return "WARNING";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  case LOG_LEVEL_TRACE:
    return "TRACE";
  default:
    return "UNKNOWN";
  }
}

const char *get_log_category_string(LogCategory category) {
  switch (category) {
  case LOG_CATEGORY_GENERAL:
    return "GENERAL";
  case LOG_CATEGORY_LEXER:
    return "LEXER";
  case LOG_CATEGORY_PARSER:
    return "PARSER";
  case LOG_CATEGORY_SEMANTIC:
    return "SEMANTIC";
  case LOG_CATEGORY_IR_GEN:
    return "IR_GEN";
  case LOG_CATEGORY_IR_OPT:
    return "IR_OPT";
  case LOG_CATEGORY_BACKEND:
    return "BACKEND";
  case LOG_CATEGORY_MEMORY:
    return "MEMORY";
  case LOG_CATEGORY_PERFORMANCE:
    return "PERFORMANCE";
  case LOG_CATEGORY_SECURITY:
    return "SECURITY";
  default:
    return "UNKNOWN";
  }
}

bool parse_log_level(const char *str, LogLevel *level) {
  if (!str || !level)
    return false;

  if (strcmp(str, "none") == 0 || strcmp(str, "NONE") == 0) {
    *level = LOG_LEVEL_NONE;
  } else if (strcmp(str, "error") == 0 || strcmp(str, "ERROR") == 0) {
    *level = LOG_LEVEL_ERROR;
  } else if (strcmp(str, "warning") == 0 || strcmp(str, "WARNING") == 0) {
    *level = LOG_LEVEL_WARNING;
  } else if (strcmp(str, "info") == 0 || strcmp(str, "INFO") == 0) {
    *level = LOG_LEVEL_INFO;
  } else if (strcmp(str, "debug") == 0 || strcmp(str, "DEBUG") == 0) {
    *level = LOG_LEVEL_DEBUG;
  } else if (strcmp(str, "trace") == 0 || strcmp(str, "TRACE") == 0) {
    *level = LOG_LEVEL_TRACE;
  } else {
    return false;
  }

  return true;
}

bool parse_log_category(const char *str, LogCategory *category) {
  if (!str || !category)
    return false;

  if (strcmp(str, "general") == 0 || strcmp(str, "GENERAL") == 0) {
    *category = LOG_CATEGORY_GENERAL;
  } else if (strcmp(str, "lexer") == 0 || strcmp(str, "LEXER") == 0) {
    *category = LOG_CATEGORY_LEXER;
  } else if (strcmp(str, "parser") == 0 || strcmp(str, "PARSER") == 0) {
    *category = LOG_CATEGORY_PARSER;
  } else if (strcmp(str, "semantic") == 0 || strcmp(str, "SEMANTIC") == 0) {
    *category = LOG_CATEGORY_SEMANTIC;
  } else if (strcmp(str, "ir_gen") == 0 || strcmp(str, "IR_GEN") == 0) {
    *category = LOG_CATEGORY_IR_GEN;
  } else if (strcmp(str, "ir_opt") == 0 || strcmp(str, "IR_OPT") == 0) {
    *category = LOG_CATEGORY_IR_OPT;
  } else if (strcmp(str, "backend") == 0 || strcmp(str, "BACKEND") == 0) {
    *category = LOG_CATEGORY_BACKEND;
  } else if (strcmp(str, "memory") == 0 || strcmp(str, "MEMORY") == 0) {
    *category = LOG_CATEGORY_MEMORY;
  } else if (strcmp(str, "performance") == 0 ||
             strcmp(str, "PERFORMANCE") == 0) {
    *category = LOG_CATEGORY_PERFORMANCE;
  } else if (strcmp(str, "security") == 0 || strcmp(str, "SECURITY") == 0) {
    *category = LOG_CATEGORY_SECURITY;
  } else {
    return false;
  }

  return true;
}

// --- 增强的日志记录函数 ---

void logger_vlog(const LogConfig *config, LogLevel level, LogCategory category,
                 const char *file, int line, const char *format, va_list args) {
  if (!config || level > config->level)
    return;
  if (!logger_is_category_enabled(config, category))
    return;

  char time_buffer[26] = "";
  if (config->enable_timestamps) {
    time_t timer = time(NULL);
    struct tm *tm_info = localtime(&timer);
    strftime(time_buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    set_log_color(config, LOG_COLOR_WHITE, stderr);
    fprintf(stderr, "[%s] ", time_buffer);
  }

  set_log_color(config, get_log_level_color(level), stderr);
  fprintf(stderr, "[%-7s]", get_log_level_string(level));

  if (config->enable_categories) {
    set_log_color(config, get_log_category_color(category), stderr);
    fprintf(stderr, "[%-11s]", get_log_category_string(category));
  }

  if (config->enable_file_line) {
    set_log_color(config, LOG_COLOR_CYAN, stderr);
    fprintf(stderr, "[%s:%d]", file, line);
  }

  fprintf(stderr, ": ");
  reset_log_color(config, stderr);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  fflush(stderr);
}

void logger_log(const LogConfig *config, LogLevel level, LogCategory category,
                const char *file, int line, const char *format, ...) {
  va_list args;
  va_start(args, format);
  logger_vlog(config, level, category, file, line, format, args);
  va_end(args);
}