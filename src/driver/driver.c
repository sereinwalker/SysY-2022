#include "ast.h"
#include "error.h"
#include "logger.h"
#include "parser.tab.h"
#include "semantic_analyzer.h"
#include "ir/ir.h" // Re-enable IR header
#include "backend_riscv.h"
#include <stdio.h>
#include <stdbool.h>                // for true, false, bool
#include <string.h>
#include <libgen.h> // For basename
#include "ir/ir_data_structures.h"  // for IRModule
#include "scanner_context.h"
typedef void* yyscan_t;
int yylex_init(yyscan_t* scanner);
void yyset_in(FILE* in_str, yyscan_t scanner);

// Defined in lexer.l, used for parser to access input file handle
extern FILE* yyin;

// Global AST Context, accessible by parser and lexer
ASTContext* parser_ctx_g = NULL;

// --- Missing type definitions ---
typedef enum {
    STAGE_LEXER,
    STAGE_PARSER,
    STAGE_SEMANTIC,
    STAGE_IR_GEN,
    STAGE_ASM
} CompilerStage;

// --- Missing function declarations ---
static bool has_errors(const ErrorContext* errors) {
    return errors && errors->count > 0;
}

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [options] <input_file>\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <output_file>  Specify output file name (default: a.s)\n");
    fprintf(stderr, "  -S                Emit optimized LLVM IR (.ll) instead of assembly\n");
    fprintf(stderr, "  -v, --verbose     Enable verbose logging (DEBUG level)\n");
    fprintf(stderr, "  -t, --trace       Enable trace logging (TRACE level)\n");
    fprintf(stderr, "  --log-level <level>  Set specific log level (none|error|warning|info|debug|trace)\n");
    fprintf(stderr, "  --log-category <cat> Enable specific log category\n");
    fprintf(stderr, "  --no-timestamps   Disable timestamps in log output\n");
    fprintf(stderr, "  --no-categories   Disable category prefixes in log output\n");
    fprintf(stderr, "  -h, --help        Display this help message\n");
}

void print_errors(const ErrorContext* errors, const char* filename) {
    if (!errors || !filename) {
        fprintf(stderr, "Error: Invalid error context or filename\n");
        return;
    }
    
    for (size_t i = 0; i < errors->count; ++i) {
        ErrorEntry* err = &errors->errors[i];
        const char* severity_str = get_error_severity_string(err->severity);
        const char* type_str = get_error_type_string(err->type);
        
        fprintf(stderr, "%s:%d:%d: %s: %s: %s\n",
                filename, err->loc.first_line, err->loc.first_column, 
                severity_str, type_str, err->message);
    }
    
    // Print summary
    size_t error_count = get_error_count_by_severity(errors, ERROR_SEVERITY_ERROR);
    size_t warning_count = get_error_count_by_severity(errors, ERROR_SEVERITY_WARNING);
    size_t fatal_count = get_error_count_by_severity(errors, ERROR_SEVERITY_FATAL);
    
    if (fatal_count > 0) {
        fprintf(stderr, "\nFATAL: %zu fatal error(s) occurred\n", fatal_count);
    }
    if (error_count > 0) {
        fprintf(stderr, "ERROR: %zu error(s) occurred\n", error_count);
    }
    if (warning_count > 0) {
        fprintf(stderr, "WARNING: %zu warning(s) occurred\n", warning_count);
    }
}

bool validate_compiler_arguments(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: No input file specified.\n");
        return false;
    }
    
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) {
            fprintf(stderr, "Error: NULL argument at position %d\n", i);
            return false;
        }
    }
    
    return true;
}

int main(int argc, char** argv) {
    char* input_filename = NULL;
    char* output_filename = "a.s";
    char* stage_str = NULL;
    bool emit_llvm = false;
    LogLevel log_level = LOG_LEVEL_INFO;
    LogConfig log_config = {0};

    // Initialize default log configuration
    logger_config_init_default(&log_config);
    log_config.enable_timestamps = true;
    log_config.enable_categories = true;
    log_config.enable_file_line = true;

    // --- Argument Parsing (Options First) ---
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == NULL) continue; // Skip NULL args

        if (strcmp(argv[i], "-o") == 0) {
            if (++i < argc) {
                output_filename = argv[i];
                argv[i] = NULL; // Mark as consumed
            } else {
                LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: -o option requires an argument.");
                return 1;
            }
            argv[i-1] = NULL; // Mark as consumed
        } else if (strcmp(argv[i], "-S") == 0) {
            emit_llvm = true;
            argv[i] = NULL;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            log_level = LOG_LEVEL_DEBUG;
            argv[i] = NULL;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--trace") == 0) {
            log_level = LOG_LEVEL_TRACE;
            argv[i] = NULL;
        } else if (strcmp(argv[i], "--log-level") == 0) {
            if (++i < argc) {
                LogLevel parsed_level;
                if (parse_log_level(argv[i], &parsed_level)) {
                    log_level = parsed_level;
                } else {
                    LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: Invalid log level '%s'", argv[i]);
                    return 1;
                }
                argv[i] = NULL;
            } else {
                LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: --log-level option requires an argument.");
                return 1;
            }
            argv[i-1] = NULL;
        } else if (strcmp(argv[i], "--log-category") == 0) {
            if (++i < argc) {
                LogCategory category;
                if (parse_log_category(argv[i], &category)) {
                    // Enable specific category by adding it to enabled_categories array
                    if (log_config.enabled_category_count < 10) {
                        log_config.enabled_categories[log_config.enabled_category_count++] = category;
                        log_config.categories_explicitly_set = true;
                    }
                } else {
                    LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: Invalid log category '%s'", argv[i]);
                    return 1;
                }
                argv[i] = NULL;
            } else {
                LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: --log-category option requires an argument.");
                return 1;
            }
            argv[i-1] = NULL;
        } else if (strcmp(argv[i], "--no-timestamps") == 0) {
            log_config.enable_timestamps = false;
            argv[i] = NULL;
        } else if (strcmp(argv[i], "--no-categories") == 0) {
            log_config.enable_categories = false;
            argv[i] = NULL;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(basename(argv[0]));
            return 0;
        }
    }

    // --- Argument Parsing (Positional Arguments) ---
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == NULL) continue; // Skip consumed options

        if (argv[i][0] == '-') {
             LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: Unknown option '%s'.", argv[i]);
             print_usage(basename(argv[0]));
             return 1;
        }

        if (stage_str == NULL) {
            stage_str = argv[i];
        } else if (input_filename == NULL) {
            input_filename = argv[i];
        } else {
            LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: Multiple input files specified ('%s' and '%s').", input_filename, argv[i]);
            return 1;
        }
    }
    
    if (input_filename == NULL || stage_str == NULL) {
        LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Error: Missing stage or input file.");
        print_usage(basename(argv[0]));
        return 1;
    }

    // Set the log level in the config
    log_config.level = log_level;

    // --- Stage Selection ---
    if (strcmp(stage_str, "lexer") == 0) {
        // stage = STAGE_LEXER;
    } else if (strcmp(stage_str, "parser") == 0) {
        // stage = STAGE_PARSER;
    } else if (strcmp(stage_str, "semantic") == 0) {
        // stage = STAGE_SEMANTIC;
    } else if (strcmp(stage_str, "ir_gen") == 0) {
        // stage = STAGE_IR_GEN;
    } else if (strcmp(stage_str, "asm") == 0) {
        // stage = STAGE_ASM;
    } else {
        LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "Unknown stage: %s", stage_str);
        return 1;
    }

    // --- Phase 1: Parsing ---
    LOG_INFO(&log_config, LOG_CATEGORY_PARSER, "Starting Phase 1: Parsing '%s'", input_filename);
    yyin = fopen(input_filename, "r");
    if (!yyin) {
        LOG_ERROR(&log_config, LOG_CATEGORY_GENERAL, "FATAL: Could not open input file: %s", input_filename);
        return 1;
    }

    parser_ctx_g = create_ast_context();
    if (!parser_ctx_g) {
        LOG_ERROR(&log_config, LOG_CATEGORY_MEMORY, "FATAL: Failed to create AST context");
        fclose(yyin);
        return 1;
    }
    
    // ErrorContext 由 create_ast_context 自动初始化，无需手动调用
    
    yyscan_t scanner;
    yylex_init(&scanner);
    yyset_in(yyin, scanner);

    ScannerContext scanner_ctx = {0};
    scanner_ctx.ast_ctx = parser_ctx_g;
    int pres = yyparse(scanner, &scanner_ctx);

    yylex_destroy(scanner);
    fclose(yyin);

    if (pres != 0 || has_errors(&parser_ctx_g->errors)) {
        LOG_ERROR(&log_config, LOG_CATEGORY_PARSER, "Compilation failed during parsing phase.");
        print_errors(&parser_ctx_g->errors, input_filename);
        destroy_ast_context(parser_ctx_g);
        return 1;
    }
    LOG_INFO(&log_config, LOG_CATEGORY_PARSER, "Parsing completed successfully. AST generated.");

    // --- Phase 2: Semantic Analysis ---
    LOG_INFO(&log_config, LOG_CATEGORY_SEMANTIC, "Starting Phase 2: Semantic Analysis");
    perform_semantic_analysis(parser_ctx_g);
    if (parser_ctx_g->errors.count > 0) {
        LOG_ERROR(&log_config, LOG_CATEGORY_SEMANTIC, "Compilation failed during semantic analysis.");
        print_errors(&parser_ctx_g->errors, input_filename);
        destroy_ast_context(parser_ctx_g);
        return 1;
    }
    LOG_INFO(&log_config, LOG_CATEGORY_SEMANTIC, "Semantic analysis successful. No errors found.");

    // --- Intermediate File Naming ---
    char optimized_ir_file[] = "temp.opt.ll";

    // --- Phase 3: Manual IR Generation ---
    LOG_INFO(&log_config, LOG_CATEGORY_IR_GEN, "Starting Phase 3: Manual IR Generation");
    IRModule* module = generate_ir(parser_ctx_g);
    if (!module) {
        LOG_ERROR(&log_config, LOG_CATEGORY_IR_GEN, "Error: Manual IR generation failed.");
        print_errors(&parser_ctx_g->errors, input_filename);
        destroy_ast_context(parser_ctx_g);
        return 1;
    }
    LOG_INFO(&log_config, LOG_CATEGORY_IR_GEN, "Manual IR generation completed.");

    // --- Phase 4: Manual IR Optimization ---
    LOG_INFO(&log_config, LOG_CATEGORY_IR_OPT, "Starting Phase 4: Manual IR Optimization");
    if (!optimize_ir(module, optimized_ir_file)) {
        LOG_ERROR(&log_config, LOG_CATEGORY_IR_OPT, "Error: Manual IR optimization failed.");
        destroy_ir_module(module);
        destroy_ast_context(parser_ctx_g);
        return 1;
    }
    LOG_INFO(&log_config, LOG_CATEGORY_IR_OPT, "Manual IR optimization completed -> '%s'", optimized_ir_file);

    // --- Phase 5: Backend Code Generation ---
    if (emit_llvm) {
        // If user just wants the LLVM IR, rename the optimized file
        if (rename(optimized_ir_file, output_filename) != 0) {
            LOG_ERROR(&log_config, LOG_CATEGORY_BACKEND, "Error renaming optimized IR file");
            remove(optimized_ir_file);
        } else {
            LOG_INFO(&log_config, LOG_CATEGORY_BACKEND, "Optimized LLVM IR emitted to '%s'.", output_filename);
        }
    } else {
        // Otherwise, generate final assembly from the optimized IR
        LOG_INFO(&log_config, LOG_CATEGORY_BACKEND, "Starting Phase 5: Backend Assembly Generation");
        if (generate_riscv_assembly(module, output_filename) != 0) {
            LOG_ERROR(&log_config, LOG_CATEGORY_BACKEND, "Error: Backend assembly generation failed.");
            destroy_ir_module(module);
            destroy_ast_context(parser_ctx_g);
            remove(optimized_ir_file);
            return 1;
        }
        LOG_INFO(&log_config, LOG_CATEGORY_BACKEND, "Assembly generation completed successfully -> '%s'", output_filename);
    }

    // --- Cleanup ---
    destroy_ir_module(module);
    destroy_ast_context(parser_ctx_g);
    if (!emit_llvm) {
        remove(optimized_ir_file);
    }
    
    LOG_INFO(&log_config, LOG_CATEGORY_GENERAL, "Compilation finished successfully.");
    return 0;
}