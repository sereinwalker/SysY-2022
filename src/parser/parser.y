%{
/**
 * @file parser.y
 * @brief Parser for the SysY language, implemented with Bison. (Reentrant Version)
 * @details This version uses a pure-parser interface, passing all state via a
 *          custom ScannerContext. It includes robust memory management for lists
 *          using a memory pool.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "ast.h"
#include "error.h"
#include "scanner_context.h"
#include "location.h"

typedef struct DefItem {
    char* name;
    ASTNodeList* dims;
    ASTNode* init;
    SourceLocation loc;
} DefItem;

typedef struct DefList {
    DefItem** items;
    size_t count;
    size_t capacity;
} DefList;

/*
 * All helper functions are adapted to receive the ScannerContext* ctx
 * to access the memory pool and other state in a reentrant-safe way.
*/

// --- Adapted C Helper Functions ---

static DefItem* create_def_item(ScannerContext* ctx, char* name, ASTNodeList* dims, ASTNode* init, SourceLocation loc) {
    DefItem* item = (DefItem*)pool_alloc(ctx->ast_ctx->pool, sizeof(DefItem));
    item->name = name;
    item->dims = dims;
    item->init = init;
    item->loc = loc;
    return item;
}

static DefList* create_def_list(ScannerContext* ctx) {
    DefList* list = (DefList*)pool_alloc(ctx->ast_ctx->pool, sizeof(DefList));
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    return list;
}

static void add_to_def_list(ScannerContext* ctx, DefList* list, DefItem* item) {
    if (!list || !item) return;
    if (list->count >= list->capacity) {
        size_t old_capacity_in_bytes = list->capacity * sizeof(DefItem*);
        list->capacity = (list->capacity == 0) ? 8 : list->capacity * 2;
        DefItem** new_items = (DefItem**)pool_alloc(ctx->ast_ctx->pool, list->capacity * sizeof(DefItem*));
        if (list->items) {
            memcpy(new_items, list->items, old_capacity_in_bytes);
        }
        list->items = new_items;
    }
    list->items[list->count++] = item;
}

static ASTNodeList* create_node_list(ScannerContext* ctx) {
    ASTNodeList* list = (ASTNodeList*)pool_alloc(ctx->ast_ctx->pool, sizeof(ASTNodeList));
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    return list;
}

static void add_to_node_list(ScannerContext* ctx, ASTNodeList* list, ASTNode* node) {
    if (!list) return;
    if (list->count >= list->capacity) {
        size_t old_capacity_in_bytes = list->capacity * sizeof(ASTNode*);
        list->capacity = (list->capacity == 0) ? 8 : list->capacity * 2;
        ASTNode** new_items = (ASTNode**)pool_alloc(ctx->ast_ctx->pool, list->capacity * sizeof(ASTNode*));
        if (list->items) {
            memcpy(new_items, list->items, old_capacity_in_bytes);
        }
        list->items = new_items;
    }
    list->items[list->count++] = node;
}

static Type* build_final_type(ScannerContext* ctx, Type* base_type, ASTNodeList* dims, bool is_const) {
    assert(base_type && dims && dims->count > 0);
    Type* new_base_type = (Type*)pool_alloc(ctx->ast_ctx->pool, sizeof(Type));
    memcpy(new_base_type, base_type, sizeof(Type));
    new_base_type->is_const = false;
    ArrayDimension* dim_info = (ArrayDimension*)pool_alloc(ctx->ast_ctx->pool, dims->count * sizeof(ArrayDimension));
    for (size_t i = 0; i < dims->count; i++) {
        dim_info[i].dim_expr = dims->items[i];
        dim_info[i].is_dynamic = (dims->items[i] == NULL);
        dim_info[i].static_size = -1;
    }
    return create_array_type(new_base_type, dim_info, dims->count, is_const, ctx->ast_ctx->pool);
}

%}

%code requires {
    #include <stddef.h>
    #include <stdint.h>
    typedef void* yyscan_t;
    typedef struct ASTNodeList ASTNodeList;
    typedef struct DefItem DefItem;
    typedef struct DefList DefList;
    typedef struct ScannerContext ScannerContext;
    typedef struct SourceLocation SourceLocation;
}

%code provides {
    int yylex(YYSTYPE *yylval, YYLTYPE *yylloc, yyscan_t yyscanner);
    void yyerror(const YYLTYPE* loc, yyscan_t yyscanner, ScannerContext* ctx, const char* msg);
}

// -- Bison Configuration --

%define api.pure full
%locations
%define parse.trace
%define api.location.type {SourceLocation}

// Define extra parameters for yyparse and yylex.
%parse-param { yyscan_t scanner }
%parse-param { ScannerContext* ctx }
%lex-param   { yyscan_t scanner }

// Define the union of possible semantic values.
%union {
    int32_t int_val;
    float float_val;
    struct ASTNode* ast_node;
    struct Type* type;
    ASTNodeList* node_list;
    DefItem* def_item;
    DefList* def_list;
}

// -- Token and Nonterminal Declarations --
%token <int_val> INT_CONST
%token <float_val> FLOAT_CONST
%token <ast_node> IDENTIFIER STRING_LITERAL ERROR
%token CONST INT FLOAT VOID RETURN IF ELSE WHILE BREAK CONTINUE
%token ADD SUB MUL DIV MOD EQ NE LT LE GT GE AND OR NOT ASSIGN LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET SEMICOLON COMMA

%type <ast_node> comp_unit func_def block stmt exp_stmt if_stmt while_stmt return_stmt break_stmt continue_stmt assign_stmt exp const_init_val init_val lval primary_exp number const_exp cond mul_exp add_exp rel_exp eq_exp l_and_exp l_or_exp func_arg unary_exp postfix_exp param_decl
%type <node_list> comp_item_list decl func_fparams block_item_list func_rparams array_dims const_init_list init_list subsequent_dims const_decl var_decl
%type <node_list> non_empty_const_init_list non_empty_init_list // 新增: 用于支持尾部逗号的非终结符
%type <type> type_specifier
%type <def_item> const_def var_def
%type <def_list> const_def_list var_def_list

// -- Operator Precedence and Associativity --
%left OR
%left AND
%left EQ NE
%left LT LE GT GE
%left ADD SUB
%left MUL DIV MOD
%right NOT UMINUS
%nonassoc IF_WITHOUT_ELSE
%nonassoc ELSE

// -- Grammar Rules --
%start comp_unit
%%

comp_unit: comp_item_list { 
    SourceLocation loc = { .first_line = 1, .first_column = 1, .last_line = 1, .last_column = 1 };
    if ($1 && $1->count > 0 && $1->items[0]) {
        loc.last_line = $1->items[$1->count - 1]->loc.last_line;
        loc.last_column = $1->items[$1->count - 1]->loc.last_column;
    }
    $$ = create_compound_stmt(ctx->ast_ctx, $1->items, $1->count, loc); 
    ctx->ast_ctx->root = $$;
};

comp_item_list: /* empty */ { $$ = create_node_list(ctx); }
    | comp_item_list decl { if ($2) { for(size_t i=0; i<$2->count; ++i) add_to_node_list(ctx, $1, $2->items[i]); } $$ = $1; }
    | comp_item_list func_def { add_to_node_list(ctx, $1, $2); $$ = $1; }
    | comp_item_list error SEMICOLON { yyerrok; }
    ;

decl: const_decl { $$ = $1; } | var_decl { $$ = $1; };

type_specifier: INT { $$ = create_basic_type(BASIC_INT, false, ctx->ast_ctx->pool); }
    | FLOAT { $$ = create_basic_type(BASIC_FLOAT, false, ctx->ast_ctx->pool); }
    | VOID { $$ = create_void_type(ctx->ast_ctx->pool); };

array_dims: /* empty */ { $$ = create_node_list(ctx); }
    | array_dims LBRACKET const_exp RBRACKET { add_to_node_list(ctx, $1, $3); $$ = $1; };

const_decl: CONST type_specifier const_def_list SEMICOLON {
    ASTNodeList* final_nodes = create_node_list(ctx);
    for (size_t i = 0; i < $3->count; i++) {
        DefItem* item = $3->items[i];
        Type* final_type;
        if (item->dims && item->dims->count > 0) {
            final_type = build_final_type(ctx, $2, item->dims, true);
        } else {
            final_type = (Type*)pool_alloc(ctx->ast_ctx->pool, sizeof(Type));
            memcpy(final_type, $2, sizeof(Type));
            final_type->is_const = true;
        }
        ASTNode* decl_node = create_const_decl(ctx->ast_ctx, item->name, final_type, item->init, item->loc);
        add_to_node_list(ctx, final_nodes, decl_node);
    }
    $$ = final_nodes;
};

const_def_list: const_def { $$ = create_def_list(ctx); add_to_def_list(ctx, $$, $1); }
    | const_def_list COMMA const_def { add_to_def_list(ctx, $1, $3); $$ = $1; };

const_def: IDENTIFIER array_dims ASSIGN const_init_val {
    $$ = create_def_item(ctx, $1->identifier.name, $2, $4, @1);
};

var_decl: type_specifier var_def_list SEMICOLON {
    ASTNodeList* final_nodes = create_node_list(ctx);
    for (size_t i = 0; i < $2->count; i++) {
        DefItem* item = $2->items[i];
        Type* final_type;
        if (item->dims && item->dims->count > 0) {
            final_type = build_final_type(ctx, $1, item->dims, false);
        } else {
            final_type = (Type*)pool_alloc(ctx->ast_ctx->pool, sizeof(Type));
            memcpy(final_type, $1, sizeof(Type));
            final_type->is_const = false;
        }
        ASTNode* decl_node = create_var_decl(ctx->ast_ctx, item->name, final_type, item->init, item->loc);
        add_to_node_list(ctx, final_nodes, decl_node);
    }
    $$ = final_nodes;
};

var_def_list: var_def { $$ = create_def_list(ctx); add_to_def_list(ctx, $$, $1); }
    | var_def_list COMMA var_def { add_to_def_list(ctx, $1, $3); $$ = $1; };

var_def: IDENTIFIER array_dims { $$ = create_def_item(ctx, $1->identifier.name, $2, NULL, @1); }
     | IDENTIFIER array_dims ASSIGN init_val { $$ = create_def_item(ctx, $1->identifier.name, $2, $4, @1); };

const_init_val: const_exp { $$ = $1; }
    | LBRACE RBRACE { $$ = create_array_init(ctx->ast_ctx, NULL, 0, @$); }
    | LBRACE const_init_list RBRACE { $$ = create_array_init(ctx->ast_ctx, $2->items, $2->count, @$); };

// 修改: 重构列表规则以支持可选的尾部逗号
const_init_list: non_empty_const_init_list { $$ = $1; }
               | non_empty_const_init_list COMMA { $$ = $1; };

non_empty_const_init_list: const_init_val { $$ = create_node_list(ctx); add_to_node_list(ctx, $$, $1); }
    | non_empty_const_init_list COMMA const_init_val { add_to_node_list(ctx, $1, $3); $$ = $1; };

init_val: exp { $$ = $1; }
    | LBRACE RBRACE { $$ = create_array_init(ctx->ast_ctx, NULL, 0, @$); }
    | LBRACE init_list RBRACE { $$ = create_array_init(ctx->ast_ctx, $2->items, $2->count, @$); };

// 修改: 重构列表规则以支持可选的尾部逗号
init_list: non_empty_init_list { $$ = $1; }
         | non_empty_init_list COMMA { $$ = $1; };

non_empty_init_list: init_val { $$ = create_node_list(ctx); add_to_node_list(ctx, $$, $1); }
    | non_empty_init_list COMMA init_val { add_to_node_list(ctx, $1, $3); $$ = $1; };

func_def: type_specifier IDENTIFIER LPAREN func_fparams RPAREN block { $$ = create_func_decl(ctx->ast_ctx, $2->identifier.name, $1, $4->items, $4->count, $6, @$); }
    | type_specifier IDENTIFIER LPAREN RPAREN block { $$ = create_func_decl(ctx->ast_ctx, $2->identifier.name, $1, NULL, 0, $5, @$); };

func_fparams: param_decl { $$ = create_node_list(ctx); add_to_node_list(ctx, $$, $1); }
    | func_fparams COMMA param_decl { add_to_node_list(ctx, $1, $3); $$ = $1; };

param_decl: type_specifier IDENTIFIER {
    /* 标量参数，不变 */
    Type* final_type = (Type*)pool_alloc(ctx->ast_ctx->pool, sizeof(Type));
    memcpy(final_type, $1, sizeof(Type));
    final_type->is_const = false;
    $$ = create_func_param(ctx->ast_ctx, $2->identifier.name, final_type, @$);
} | type_specifier IDENTIFIER LBRACKET RBRACKET subsequent_dims {
    /* 数组参数 */
    ASTNodeList* all_dims = create_node_list(ctx);
    add_to_node_list(ctx, all_dims, NULL);
    for (size_t i = 0; i < $5->count; ++i) {
        add_to_node_list(ctx, all_dims, $5->items[i]);
    }
    Type* final_type = build_final_type(ctx, $1, all_dims, false);
    $$ = create_func_param(ctx->ast_ctx, $2->identifier.name, final_type, @$);
};

subsequent_dims: /* empty */ { $$ = create_node_list(ctx); }
    | subsequent_dims LBRACKET const_exp RBRACKET { add_to_node_list(ctx, $1, $3); $$ = $1; };

block: LBRACE block_item_list RBRACE { $$ = create_compound_stmt(ctx->ast_ctx, $2->items, $2->count, @$); };

block_item_list: /* empty */ { $$ = create_node_list(ctx); }
    | block_item_list decl { if ($2) { for(size_t i=0; i<$2->count; ++i) add_to_node_list(ctx, $1, $2->items[i]); } $$ = $1; }
    | block_item_list stmt { add_to_node_list(ctx, $1, $2); $$ = $1; }
    | block_item_list error SEMICOLON { yyerrok; }
    ;

stmt: assign_stmt | exp_stmt | block | if_stmt | while_stmt | break_stmt | continue_stmt | return_stmt;

lval: IDENTIFIER { $$ = $1; }
    | lval LBRACKET exp RBRACKET { $$ = create_array_access(ctx->ast_ctx, $1, $3, @$); };

assign_stmt: lval ASSIGN exp SEMICOLON { $$ = create_assign_stmt(ctx->ast_ctx, $1, $3, @$); };

exp_stmt: exp SEMICOLON { $$ = create_expr_stmt(ctx->ast_ctx, $1, @$); }
    | SEMICOLON { $$ = create_expr_stmt(ctx->ast_ctx, NULL, @$); };

if_stmt: IF LPAREN cond RPAREN stmt %prec IF_WITHOUT_ELSE { $$ = create_if_stmt(ctx->ast_ctx, $3, $5, NULL, @$); }
    | IF LPAREN cond RPAREN stmt ELSE stmt { $$ = create_if_stmt(ctx->ast_ctx, $3, $5, $7, @$); };

while_stmt: WHILE LPAREN cond RPAREN stmt { $$ = create_while_stmt(ctx->ast_ctx, $3, $5, @$); };

break_stmt: BREAK SEMICOLON { $$ = create_break_stmt(ctx->ast_ctx, @$); };

continue_stmt: CONTINUE SEMICOLON { $$ = create_continue_stmt(ctx->ast_ctx, @$); };

return_stmt: RETURN SEMICOLON { $$ = create_return_stmt(ctx->ast_ctx, NULL, @$); }
    | RETURN exp SEMICOLON { $$ = create_return_stmt(ctx->ast_ctx, $2, @$); };

exp: add_exp;
cond: l_or_exp;
const_exp: add_exp;

number: INT_CONST { $$ = create_int_constant(ctx->ast_ctx, $1, @$); }
    | FLOAT_CONST { $$ = create_float_constant(ctx->ast_ctx, $1, @$); };

primary_exp: lval | number | LPAREN exp RPAREN { $$ = $2; };

postfix_exp: primary_exp
    | postfix_exp LPAREN RPAREN { $$ = create_call_expr(ctx->ast_ctx, $1, NULL, 0, @$); }
    | postfix_exp LPAREN func_rparams RPAREN { $$ = create_call_expr(ctx->ast_ctx, $1, $3->items, $3->count, @$); };

unary_exp: postfix_exp
    | ADD unary_exp %prec UMINUS { $$ = create_unary_expr(ctx->ast_ctx, OP_POS, $2, @$); }
    | SUB unary_exp %prec UMINUS { $$ = create_unary_expr(ctx->ast_ctx, OP_NEG, $2, @$); }
    | NOT unary_exp { $$ = create_unary_expr(ctx->ast_ctx, OP_NOT, $2, @$); };

mul_exp: unary_exp
    | mul_exp MUL unary_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_MUL, $1, $3, @$); }
    | mul_exp DIV unary_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_DIV, $1, $3, @$); }
    | mul_exp MOD unary_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_MOD, $1, $3, @$); };

add_exp: mul_exp
    | add_exp ADD mul_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_ADD, $1, $3, @$); }
    | add_exp SUB mul_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_SUB, $1, $3, @$); };

rel_exp: add_exp
    | rel_exp LT add_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_LT, $1, $3, @$); }
    | rel_exp GT add_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_GT, $1, $3, @$); }
    | rel_exp LE add_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_LE, $1, $3, @$); }
    | rel_exp GE add_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_GE, $1, $3, @$); };

eq_exp: rel_exp
    | eq_exp EQ rel_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_EQ, $1, $3, @$); }
    | eq_exp NE rel_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_NE, $1, $3, @$); };

l_and_exp: eq_exp | l_and_exp AND eq_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_AND, $1, $3, @$); };

l_or_exp: l_and_exp | l_or_exp OR l_and_exp { $$ = create_binary_expr(ctx->ast_ctx, OP_OR, $1, $3, @$); };

func_arg: exp | STRING_LITERAL;

func_rparams: func_arg { $$ = create_node_list(ctx); add_to_node_list(ctx, $$, $1); }
    | func_rparams COMMA func_arg { add_to_node_list(ctx, $1, $3); $$ = $1; };

%%

// -- C Code (Epilogue) --

/**
 * @brief Bison's error reporting function for the reentrant parser.
 * @param loc The location of the error.
 * @param scanner The Flex scanner instance (unused here, but required by the signature).
 * @param ctx Our custom context.
 * @param msg The generic error message from Bison.
 */
void yyerror(const YYLTYPE* loc, yyscan_t yyscanner, ScannerContext* ctx, const char* msg) {
    (void)yyscanner; // Suppress unused parameter warning.
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", msg);
    add_error(&ctx->ast_ctx->errors, ERROR_SYNTAX, buffer, *loc);
}