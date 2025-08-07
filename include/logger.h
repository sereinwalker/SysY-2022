#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdarg.h> // 用于 va_list
#include <stdio.h>  // 用于 FILE

/**
 * @file logger.h
 * @brief 为编译器定义了一个简单、灵活的日志记录工具。
 * @details
 * 该模块提供了一个基础的日志框架，以帮助调试和跟踪编译器的执行流程。
 * 它支持多种日志级别（例如 DEBUG, INFO, WARNING, ERROR），并允许轻松控制
 * 显示哪些消息。
 *
 * 日志记录器现在基于上下文配置，每个调用者可以有自己的日志配置，
 * 完全消除了全局状态依赖，并支持彩色输出。
 */

/**
 * @enum LogLevel
 * @brief 定义了不同级别的日志详细程度。
 */
typedef enum {
    LOG_LEVEL_NONE = 0, ///< 不记录任何日志。
    LOG_LEVEL_ERROR,    ///< 仅记录严重错误。
    LOG_LEVEL_WARNING,  ///< 记录警告和错误。
    LOG_LEVEL_INFO,     ///< 记录信息性消息、警告和错误。
    LOG_LEVEL_DEBUG,    ///< 记录详细的调试消息及所有更低级别。
    LOG_LEVEL_TRACE     ///< 记录用于深度调试的非常详细的跟踪消息。
} LogLevel;

/**
 * @enum LogCategory
 * @brief 定义了不同类别的日志消息，以便更好地组织。
 */
typedef enum {
    LOG_CATEGORY_GENERAL,      ///< 通用编译器消息
    LOG_CATEGORY_LEXER,        ///< 词法分析消息
    LOG_CATEGORY_PARSER,       ///< 语法分析消息
    LOG_CATEGORY_SEMANTIC,     ///< 语义分析消息
    LOG_CATEGORY_IR_GEN,       ///< IR 生成消息
    LOG_CATEGORY_IR_OPT,       ///< IR 优化消息
    LOG_CATEGORY_BACKEND,      ///< 后端代码生成消息
    LOG_CATEGORY_MEMORY,       ///< 内存管理消息
    LOG_CATEGORY_PERFORMANCE,  ///< 性能相关消息
    LOG_CATEGORY_SECURITY      ///< 安全相关消息
} LogCategory;

/**
 * @enum LogColor
 * @brief 定义了日志输出中可用的颜色。
 */
typedef enum {
    LOG_COLOR_RESET = 0,       ///< 重置颜色
    LOG_COLOR_RED,             ///< 红色
    LOG_COLOR_GREEN,           ///< 绿色
    LOG_COLOR_YELLOW,          ///< 黄色
    LOG_COLOR_BLUE,            ///< 蓝色
    LOG_COLOR_MAGENTA,         ///< 洋红色
    LOG_COLOR_CYAN,            ///< 青色
    LOG_COLOR_WHITE,           ///< 白色
    LOG_COLOR_BRIGHT_RED,      ///< 亮红色
    LOG_COLOR_BRIGHT_GREEN,    ///< 亮绿色
    LOG_COLOR_BRIGHT_YELLOW,   ///< 亮黄色
    LOG_COLOR_BRIGHT_BLUE,     ///< 亮蓝色
    LOG_COLOR_BRIGHT_MAGENTA,  ///< 亮洋红色
    LOG_COLOR_BRIGHT_CYAN,     ///< 亮青色
    LOG_COLOR_BRIGHT_WHITE     ///< 亮白色
} LogColor;

/**
 * @struct LogConfig
 * @brief 日志记录器的配置结构体。
 */
typedef struct {
    LogLevel level;                    ///< 全局日志级别
    bool enable_timestamps;            ///< 是否包含时间戳
    bool enable_categories;            ///< 是否包含类别前缀
    bool enable_file_line;             ///< 是否包含文件:行号信息
    bool enable_colors;                ///< 是否使用彩色输出
    LogCategory enabled_categories[10];///< 要启用的类别（如果为空，则全部启用）
    int enabled_category_count;        ///< 已启用类别的数量
    bool categories_explicitly_set;    ///< 类别是否已被显式设置
} LogConfig;

// --- 日志记录器配置 ---

/**
 * @brief 初始化一个日志配置结构体为默认值。
 * @param config 指向要初始化的 LogConfig 对象的指针。
 */
void logger_config_init_default(LogConfig* config);

// --- 增强的日志记录函数 ---

/**
 * @brief 记录一条格式化消息，依赖传入的配置。
 * @param config 指向 LogConfig 对象的指针，用于决定是否打印以及如何格式化。
 * @param level  此消息的 LogLevel。
 * @param category 此消息的 LogCategory。
 * @param file 生成日志消息的源文件（通常是 `__FILE__`）。
 * @param line 源文件中的行号（通常是 `__LINE__`）。
 * @param format 消息的 `printf` 风格格式字符串。
 * @param ... 格式字符串的附加参数。
 */
void logger_log(const LogConfig* config, LogLevel level, LogCategory category, const char* file, int line, const char* format, ...);

/**
 * @brief 使用可变参数列表记录一条格式化消息。
 * @param config 指向 LogConfig 对象的指针，用于决定是否打印以及如何格式化。
 * @param level 此特定消息的 `LogLevel`。
 * @param category 此消息的 `LogCategory`。
 * @param file 生成日志消息的源文件。
 * @param line 源文件中的行号。
 * @param format 消息的 `printf` 风格格式字符串。
 * @param args 可变参数列表。
 */
void logger_vlog(const LogConfig* config, LogLevel level, LogCategory category, const char* file, int line, const char* format, va_list args);

// --- 用于日志记录的便捷宏 ---
// 宏现在需要接收一个配置对象的指针作为第一个参数。

#define LOG_WITH_CONFIG(config, level, category, format, ...) \
    logger_log(config, level, category, __FILE__, __LINE__, format, ##__VA_ARGS__)

#define LOG_ERROR(config, category, format, ...) \
    LOG_WITH_CONFIG(config, LOG_LEVEL_ERROR, category, format, ##__VA_ARGS__)

#define LOG_WARN(config, category, format, ...) \
    LOG_WITH_CONFIG(config, LOG_LEVEL_WARNING, category, format, ##__VA_ARGS__)

#define LOG_INFO(config, category, format, ...) \
    LOG_WITH_CONFIG(config, LOG_LEVEL_INFO, category, format, ##__VA_ARGS__)

#define LOG_DEBUG(config, category, format, ...) \
    LOG_WITH_CONFIG(config, LOG_LEVEL_DEBUG, category, format, ##__VA_ARGS__)

#define LOG_TRACE(config, category, format, ...) \
    LOG_WITH_CONFIG(config, LOG_LEVEL_TRACE, category, format, ##__VA_ARGS__)

// --- 工具函数 ---

/**
 * @brief 获取日志级别的人类可读的字符串表示。
 * @param level 要转换的日志级别。
 * @return 日志级别的字符串表示。
 */
const char* get_log_level_string(LogLevel level);

/**
 * @brief 获取日志类别的人类可读的字符串表示。
 * @param category 要转换的日志类别。
 * @return 日志类别的字符串表示。
 */
const char* get_log_category_string(LogCategory category);

/**
 * @brief 从字符串解析日志级别。
 * @param str 要解析的字符串。
 * @param level 用于存储解析结果的指针。
 * @return 如果解析成功，则返回 true，否则返回 false。
 */
bool parse_log_level(const char* str, LogLevel* level);

/**
 * @brief 从字符串解析日志类别。
 * @param str 要解析的字符串。
 * @param category 用于存储解析结果的指针。
 * @return 如果解析成功，则返回 true，否则返回 false。
 */
bool parse_log_category(const char* str, LogCategory* category);

// --- 颜色支持函数 ---

/**
 * @brief 获取日志级别对应的颜色。
 * @param level 日志级别。
 * @return 对应的颜色枚举值。
 */
LogColor get_log_level_color(LogLevel level);

/**
 * @brief 获取日志类别对应的颜色。
 * @param category 日志类别。
 * @return 对应的颜色枚举值。
 */
LogColor get_log_category_color(LogCategory category);

/**
 * @brief 获取ANSI颜色转义序列。
 * @param color 颜色枚举值。
 * @return 对应的ANSI转义序列字符串。
 */
const char* get_ansi_color_code(LogColor color);

/**
 * @brief 检查当前终端是否支持颜色输出。
 * @return 如果支持颜色输出，则返回 true，否则返回 false。
 */
bool is_color_supported();

/**
 * @brief 设置颜色输出。
 * @param config 日志配置对象。
 * @param color 要设置的颜色。
 * @param stream 输出流。
 */
void set_log_color(const LogConfig* config, LogColor color, FILE* stream);

/**
 * @brief 重置颜色输出。
 * @param config 日志配置对象。
 * @param stream 输出流。
 */
void reset_log_color(const LogConfig* config, FILE* stream);

#endif // LOGGER_H