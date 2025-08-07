#include "ast.h"
#include "error.h"
#include "parser.tab.h"
#include "scanner_context.h"
#include "semantic_analyzer.h"
#include <stdio.h>
#include <stdbool.h>

static bool has_errors(const ErrorContext* errors) {
    return errors && errors->count > 0;
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
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }
    const char* input_filename = argv[1];

    yyscan_t scanner;
    if (yylex_init(&scanner) != 0) {
        fprintf(stderr, "FATAL: Failed to initialize scanner\n");
        return 1;
    }

    ScannerContext* ctx = create_scanner_context(input_filename);
    if (!ctx) {
        fprintf(stderr, "FATAL: Failed to create scanner context\n");
        yylex_destroy(scanner);
        return 1;
    }
    yyset_extra(ctx, scanner);

    FILE* in = fopen(input_filename, "r");
    if (!in) {
        fprintf(stderr, "FATAL: Could not open input file: %s\n", input_filename);
        destroy_scanner_context(ctx);
        yylex_destroy(scanner);
        return 1;
    }
    yyset_in(in, scanner);

    int pres = yyparse(scanner, ctx);

    fclose(in);
    yylex_destroy(scanner);

    // 语法分析通过后，进行语义分析
    if (pres == 0 && !has_errors(&ctx->ast_ctx->errors)) {
        perform_semantic_analysis(ctx->ast_ctx);
    }

    int ret = 0;
    if (pres != 0 || has_errors(&ctx->ast_ctx->errors)) {
        print_errors(&ctx->ast_ctx->errors, input_filename);
        ret = 1;
    }
    destroy_scanner_context(ctx);
    return ret;
} 