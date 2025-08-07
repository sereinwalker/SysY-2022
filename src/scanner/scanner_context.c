#include "scanner_context.h"
#include <stdlib.h>
#include <string.h> // for strdup
#include "ast.h"    // for create/destroy_ast_context

#ifndef _WIN32
// strdup is POSIX but not standard C, so provide a fallback for MSVC if needed.
#include <string.h>
#else
#include <string.h>
char* strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* new_s = malloc(len);
    if (new_s == NULL) return NULL;
    return (char*)memcpy(new_s, s, len);
}
#endif

ScannerContext* create_scanner_context(const char* filename) {
    // 使用 calloc 来确保所有成员都被初始化为零/NULL
    ScannerContext* ctx = (ScannerContext*)calloc(1, sizeof(ScannerContext));
    if (!ctx) {
        fprintf(stderr, "Fatal: Failed to allocate memory for ScannerContext.\n");
        return NULL;
    }

    // 创建 AST 上下文
    ctx->ast_ctx = create_ast_context();
    if (!ctx->ast_ctx) {
        fprintf(stderr, "Fatal: Failed to create ASTContext.\n");
        free(ctx); // 清理已分配的 ScannerContext
        return NULL;
    }
    
    // 复制文件名，这样上下文就拥有了自己的文件名字符串
    if (filename) {
        ctx->filename = strdup(filename);
        if (!ctx->filename) {
             fprintf(stderr, "Fatal: Failed to duplicate filename string.\n");
             destroy_ast_context(ctx->ast_ctx);
             free(ctx);
             return NULL;
        }
    }

    // 其他成员 (string_buffer, _size, _len, block_start_loc) 已被 calloc 初始化为 0。
    // 这是一种安全且方便的默认状态。

    return ctx;
}

void destroy_scanner_context(ScannerContext* ctx) {
    if (!ctx) return;

    // 按创建的逆序进行销毁和释放

    // 1. 释放内部的 AST 上下文
    if (ctx->ast_ctx) {
        destroy_ast_context(ctx->ast_ctx);
        ctx->ast_ctx = NULL;
    }

    // 2. 释放文件名的副本
    //    (void*) 转换是为了丢弃 const 限定符，这是释放从 strdup 获得的内存的标准做法
    if (ctx->filename) {
        free((void*)ctx->filename);
        ctx->filename = NULL;
    }

    // 3. 释放用于字符串字面量的缓冲区
    if (ctx->string_buffer) {
        free(ctx->string_buffer);
        ctx->string_buffer = NULL;
    }

    // 4. 最后释放上下文结构体本身
    free(ctx);
}