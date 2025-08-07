%{
#ifndef YYLTYPE
#define YYLTYPE SourceLocation
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#include "parser.tab.h"
#include "scanner_context.h"

// Helper to manage the temporary string buffer in the context.
// ... (这部分代码保持不变) ...
static void append_to_string_buffer(ScannerContext* ctx, char c) {
    if (ctx->string_len + 1 >= (size_t)ctx->string_buffer_size) {
        ctx->string_buffer_size = (ctx->string_buffer_size == 0) ? 256 : ctx->string_buffer_size * 2;
        char* new_buffer = (char*)realloc(ctx->string_buffer, ctx->string_buffer_size);
        if (!new_buffer) {
            fprintf(stderr, "Fatal: realloc for string_buffer failed.\n");
            exit(1);
        }
        ctx->string_buffer = new_buffer;
    }
    ctx->string_buffer[ctx->string_len++] = c;
}


// Macros for easy access to the context and location pointer.
// ... (这部分代码保持不变) ...
#define YY_EXTRA_TYPE ScannerContext*
#define YY_CTX        ((ScannerContext*)yyget_extra(yyscanner))
#define YY_LLOC_P yylloc_param
#define UPDATE_LOCATION() \
    do { \
        YY_LLOC_P->first_line = YY_LLOC_P->last_line; \
        YY_LLOC_P->first_column = YY_LLOC_P->last_column; \
        for (int i = 0; yytext[i] != '\0'; i++) { \
            if (yytext[i] == '\n') { \
                YY_LLOC_P->last_line++; \
                YY_LLOC_P->last_column = 1; \
            } else { \
                YY_LLOC_P->last_column++; \
            } \
        } \
    } while(0)

%}

%option noyywrap nounput noinput
%option bison-bridge bison-locations reentrant
%x COMMENT
%x STRING
DIGIT       [0-9]
NONZERO     [1-9]
HEX_DIGIT   [0-9a-fA-F]
IDENT       [a-zA-Z_][a-zA-Z0-9_]*
INT_CONST   {NONZERO}{DIGIT}*|0|0[0-7]+|0[xX]{HEX_DIGIT}+
FLOAT_CONST (({DIGIT}+\.{DIGIT}*([eE][+-]?{DIGIT}+)?)|(\.{DIGIT}+([eE][+-]?{DIGIT}+)?)|({DIGIT}+[eE][+-]?{DIGIT}+))|(0[xX]({HEX_DIGIT}+\.?|{HEX_DIGIT}*\.{HEX_DIGIT}+)[pP][+-]?{DIGIT}+)

%%

%{
    char lexical_error_buffer[256];
%}

"const"     { UPDATE_LOCATION(); return CONST; }
"int"       { UPDATE_LOCATION(); return INT; }
"float"     { UPDATE_LOCATION(); return FLOAT; }
"void"      { UPDATE_LOCATION(); return VOID; }
"if"        { UPDATE_LOCATION(); return IF; }
"else"      { UPDATE_LOCATION(); return ELSE; }
"while"     { UPDATE_LOCATION(); return WHILE; }
"break"     { UPDATE_LOCATION(); return BREAK; }
"continue"  { UPDATE_LOCATION(); return CONTINUE; }
"return"    { UPDATE_LOCATION(); return RETURN; }

{IDENT} {
    UPDATE_LOCATION();
    yylval->ast_node = create_identifier(YY_CTX->ast_ctx, yytext, *YY_LLOC_P);
    return IDENTIFIER;
}

{INT_CONST} {
    UPDATE_LOCATION();
    long long val = strtoll(yytext, NULL, 0);
    if (errno == ERANGE || val > INT_MAX || val < INT_MIN) {
        snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Integer constant '%s' is out of range.", yytext);
        add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, *YY_LLOC_P);
        return ERROR;
    }
    yylval->int_val = (int32_t)val;
    return INT_CONST;
}

{FLOAT_CONST} {
    UPDATE_LOCATION();
    double val = strtod(yytext, NULL);
    if (errno == ERANGE) {
        snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Float constant '%s' is out of range.", yytext);
        add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, *YY_LLOC_P);
        return ERROR;
    }
    yylval->float_val = (float)val;
    return FLOAT_CONST;
}

"/*"        { UPDATE_LOCATION(); YY_CTX->block_start_loc = *YY_LLOC_P; BEGIN(COMMENT); }
<COMMENT>"*/" { UPDATE_LOCATION(); BEGIN(INITIAL); }
<COMMENT>.|\n      { UPDATE_LOCATION(); }
<COMMENT><<EOF>>    {
    snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Unterminated comment starting at line %d.", YY_CTX->block_start_loc.first_line);
    add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, YY_CTX->block_start_loc);
    yyterminate();
}

"//".*      { UPDATE_LOCATION(); }


"\"" {
    UPDATE_LOCATION();
    YY_CTX->block_start_loc = *YY_LLOC_P;
    YY_CTX->string_len = 0;
    BEGIN(STRING);
}

<STRING>{
    "\"" {
        UPDATE_LOCATION();
        append_to_string_buffer(YY_CTX, '\0');
        yylval->ast_node = create_string_literal(YY_CTX->ast_ctx, YY_CTX->string_buffer, YY_CTX->string_len - 1, YY_CTX->block_start_loc);
        BEGIN(INITIAL);
        return STRING_LITERAL;
    }

    "\\n"   { UPDATE_LOCATION(); append_to_string_buffer(YY_CTX, '\n'); }
    "\\t"   { UPDATE_LOCATION(); append_to_string_buffer(YY_CTX, '\t'); }
    "\\r"   { UPDATE_LOCATION(); append_to_string_buffer(YY_CTX, '\r'); }
    "\\\""  { UPDATE_LOCATION(); append_to_string_buffer(YY_CTX, '\"'); }
    "\\\\"  { UPDATE_LOCATION(); append_to_string_buffer(YY_CTX, '\\'); }

    "\\." {
        UPDATE_LOCATION();
        snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Unknown escape sequence '\\%c'.", yytext[1]);
        add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, *YY_LLOC_P);
    }
    
    "[^\"\\\n]+" {
        UPDATE_LOCATION();
        for (int i = 0; yytext[i] != '\0'; ++i) {
            append_to_string_buffer(YY_CTX, yytext[i]);
        }
    }

    "\n" {
        UPDATE_LOCATION();
        snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Unterminated string literal starting at line %d.", YY_CTX->block_start_loc.first_line);
        add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, YY_CTX->block_start_loc);
        BEGIN(INITIAL);
        yyless(0); 
        return ERROR;
    }

    <<EOF>> {
        UPDATE_LOCATION();
        snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Unterminated string literal starting at line %d.", YY_CTX->block_start_loc.first_line);
        add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, YY_CTX->block_start_loc);
        BEGIN(INITIAL);
        return ERROR;
    }
}

"+"  { UPDATE_LOCATION(); return ADD; }
"-"  { UPDATE_LOCATION(); return SUB; }
"*"  { UPDATE_LOCATION(); return MUL; }
"/"  { UPDATE_LOCATION(); return DIV; }
"%"  { UPDATE_LOCATION(); return MOD; }
"==" { UPDATE_LOCATION(); return EQ; }
"!=" { UPDATE_LOCATION(); return NE; }
"<=" { UPDATE_LOCATION(); return LE; }
">=" { UPDATE_LOCATION(); return GE; }
"<"  { UPDATE_LOCATION(); return LT; }
">"  { UPDATE_LOCATION(); return GT; }
"="  { UPDATE_LOCATION(); return ASSIGN; }
"&&" { UPDATE_LOCATION(); return AND; }
"||" { UPDATE_LOCATION(); return OR; }
"!"  { UPDATE_LOCATION(); return NOT; }
"("  { UPDATE_LOCATION(); return LPAREN; }
")"  { UPDATE_LOCATION(); return RPAREN; }
"{"  { UPDATE_LOCATION(); return LBRACE; }
"}"  { UPDATE_LOCATION(); return RBRACE; }
"["  { UPDATE_LOCATION(); return LBRACKET; }
"]"  { UPDATE_LOCATION(); return RBRACKET; }
";"  { UPDATE_LOCATION(); return SEMICOLON; }
","  { UPDATE_LOCATION(); return COMMA; }

[ \t\r\n]+  { UPDATE_LOCATION(); }
. {
    UPDATE_LOCATION();
    snprintf(lexical_error_buffer, sizeof(lexical_error_buffer), "Invalid character '%c' (ASCII: %d).", *yytext, *yytext);
    add_error(&YY_CTX->ast_ctx->errors, ERROR_LEXICAL, lexical_error_buffer, *YY_LLOC_P);
    return ERROR;
}

%%