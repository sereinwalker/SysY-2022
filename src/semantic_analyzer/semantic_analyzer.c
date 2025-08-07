/**
 * @file semantic_analyzer.c
 * @brief 实现 SysY 编译器的语义分析器。
 * @details
 * 本文件实现了语义分析的功能。它通过对抽象语法树（AST）进行两遍遍历来完成工作：
 * 1.
 * **符号表构建**：第一遍遍历AST，为每个作用域（全局、函数、代码块）构建符号表，
 *    并将所有声明（变量、常量、函数）添加到对应的作用域中，同时检查符号重定义错误。
 * 2. **语义检查**：第二遍遍历AST，执行详细的检查，包括：
 *    - 类型检查：验证赋值、运算符、函数调用等场景下的类型兼容性。
 *    - 符号解析：确保所有使用的标识符都已声明。
 *    -
 * 控制流分析：检查非void函数是否在所有执行路径上都有返回值，以及break/continue语句是否在循环内。
 *    - 常量折叠：在编译时计算常量表达式的值。
 */
#include "semantic_analyzer.h"
#include "ast.h"
#include "error.h"
#include "location.h" // for SourceLocation
#include "logger.h"
#include "symbol_table.h"
#include <stdbool.h> // for false, true, bool
#include <stdio.h>
#include <string.h>

//==============================================================================
// 1. 内部类型定义和分析上下文 (Internal Type Definitions and Analysis Context)
//==============================================================================

/**
 * @brief 语义分析上下文结构体，用于在AST遍历期间维护状态。
 */
typedef struct {
  ASTContext *ast_ctx;                ///< 指向全局AST上下文的指针
  SymbolTable *current_scope;         ///< 指向当前作用域的符号表
  Type *current_function_return_type; ///< 当前正在分析的函数的返回类型
  int loop_depth;  ///< 当前嵌套的循环深度，用于检查break/continue
  int error_count; ///< 语义错误计数
  int warning_count;         ///< 语义警告计数
  bool has_return_statement; ///< 标记当前函数体是否已包含return语句
  bool is_in_constant_context; ///< 标记当前是否在常量表达式求值上下文中
} AnalysisContext;

/**
 * @brief 用于存储常量表达式求值结果的结构体。
 */
typedef struct {
  bool is_const;         ///< 表达式是否为常量
  ConstValueUnion value; ///< 如果是常量，其值
  bool is_valid;         ///< 该求值结果是否有效
} EvaluatedConst;

/**
 * @brief 用于存储控制流分析结果的结构体。
 */
typedef struct {
  bool can_continue;     ///< True if control flow can continue *past* this
                         ///< statement.
  bool all_paths_return; ///< True if ALL possible paths through this statement
                         ///< guarantee a return from the function.
  bool has_unreachable_code; ///< True if this statement contains unreachable
                             ///< code within it.
} ControlFlowResult;

typedef struct {
  ASTNode *init_list_node; // 指向最顶层的 { ... } 初始化节点
  int current_idx; // 在扁平化视图中，当前处理到第几个元素
} InitContext;

//==============================================================================
// 2. 所有静态函数的前向声明 (Forward Declarations of All Static Functions)
//==============================================================================

// AST遍历函数
static void traverse_ast(ASTNode *node,
                         void (*pre_visit)(ASTNode *, AnalysisContext *),
                         void (*post_visit)(ASTNode *, AnalysisContext *),
                         AnalysisContext *actx);
// 符号表构建的访问函数
static void build_symbols_pre(ASTNode *node, AnalysisContext *actx);
static void build_symbols_post(ASTNode *node, AnalysisContext *actx);
// 语义检查辅助函数
static void check_array_initializer(ASTNode *init_node, Type *target_type,
                                    bool is_const_context,
                                    AnalysisContext *actx);
// MODIFIED: Simplified the signature for clarity
static bool check_and_evaluate_dimension(ASTNode *dim_expr, int *evaluated_size,
                                         AnalysisContext *actx,
                                         bool is_func_param_first_dim);
static void add_predefined_symbols(AnalysisContext *actx);
static bool check_function_call(ASTNode *call_node, AnalysisContext *actx);
static void evaluate_single_const_decl(ASTNode *node, AnalysisContext *actx);
static void check_semantics_pre(ASTNode *node, AnalysisContext *actx);
static void check_semantics_post(ASTNode *node, AnalysisContext *actx);
static bool is_constant_expression(ASTNode *node, AnalysisContext *actx);
static EvaluatedConst evaluate_const_expr_recursively(ASTNode *node,
                                                      AnalysisContext *actx);
/**
 * @brief 递归地找到数组类型最底层的基本元素类型。
 * @param type 一个数组类型。
 * @return 指向最底层元素类型的指针。
 */
static Type *get_array_base_element_type(Type *type) {
  while (type && type->kind == TYPE_ARRAY) {
    type = type->array.element_type;
  }
  return type;
}
static bool is_type_same(Type *t1, Type *t2, bool ignore_top_level_const);
static bool is_numeric_type(Type *type);
static bool is_type_compatible(Type *target, Type *source,
                               bool allow_numeric_conversion);

//==============================================================================
// 3. 语义分析主驱动函数 (Main Driver for Semantic Analysis)
//==============================================================================

void perform_semantic_analysis(ASTContext *ctx) {
  if (!ctx || !ctx->root)
    return;

  LOG_INFO(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
           "Starting semantic analysis");

  AnalysisContext actx = {.ast_ctx = ctx, .current_scope = ctx->global_scope};
  add_predefined_symbols(&actx);

  // Pass 1: 构建全局符号表和函数作用域
  LOG_DEBUG(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
            "Building symbol tables (pass 1)");
  traverse_ast(ctx->root, build_symbols_pre, build_symbols_post, &actx);

  // main 函数唯一性和签名检查 (保持不变)
  Symbol *main_sym = find_symbol_in_scope(ctx->global_scope, "main");
  if (!main_sym) {
    add_error(&ctx->errors, ERROR_UNDEFINED_VARIABLE,
              "Program must define a function 'main'",
              (SourceLocation){1, 1, 1, 1});
    LOG_ERROR(&ctx->log_config, LOG_CATEGORY_SEMANTIC, "Missing main function");
  } else if (!main_sym->is_func) {
    add_error(&ctx->errors, ERROR_TYPE_MISMATCH, "'main' must be a function",
              (SourceLocation){1, 1, 1, 1});
    LOG_ERROR(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
              "main is not a function");
  } else {
    Type *t = main_sym->type;
    if (t->function.param_count != 0 ||
        t->function.return_type->kind != TYPE_BASIC ||
        t->function.return_type->basic != BASIC_INT) {
      add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                "Function 'main' must have signature: int main()",
                (SourceLocation){1, 1, 1, 1});
      LOG_ERROR(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
                "Invalid main function signature");
    } else {
      LOG_DEBUG(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
                "Main function signature is valid");
    }
  }

  // NEW: Pass 1.5: 在进行详细语义检查前，提前求值所有全局常量
  LOG_DEBUG(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
            "Evaluating global constants (pass 1.5)");
  for (size_t i = 0; i < ctx->root->compound_stmt.item_count; ++i) {
    ASTNode *item = ctx->root->compound_stmt.items[i];
    if (item->node_type == AST_CONST_DECL) {
      evaluate_single_const_decl(item, &actx);
    }
  }

  // Pass 2: 统一的语义检查遍历
  // check_semantics_pre: 添加局部符号，进入作用域
  // check_semantics_post: 类型检查，求值常量，退出作用域
  LOG_DEBUG(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
            "Performing unified semantic checks (pass 2)");
  traverse_ast(ctx->root, check_semantics_pre, check_semantics_post, &actx);

  LOG_INFO(&ctx->log_config, LOG_CATEGORY_SEMANTIC,
           "Semantic analysis completed with %d errors, %d warnings",
           ctx->errors.count, actx.warning_count);
}

//==============================================================================
// 4. 增强的遍历实现 (Enhanced Traversal Implementation)
//==============================================================================

// 递归遍历AST节点
static void traverse_ast(ASTNode *node,
                         void (*pre_visit)(ASTNode *, AnalysisContext *),
                         void (*post_visit)(ASTNode *, AnalysisContext *),
                         AnalysisContext *actx) {
  if (!node)
    return;
  if (pre_visit)
    pre_visit(node, actx);
  switch (node->node_type) {
  case AST_COMPOUND_STMT:
    for (size_t i = 0; i < node->compound_stmt.item_count; i++) {
      traverse_ast(node->compound_stmt.items[i], pre_visit, post_visit, actx);
    }
    break;
  case AST_FUNC_DECL:
    for (size_t i = 0; i < node->func_decl.param_count; ++i) {
      traverse_ast(node->func_decl.params[i], pre_visit, post_visit, actx);
    }
    traverse_ast(node->func_decl.body, pre_visit, post_visit, actx);
    break;
  case AST_FUNC_PARAM:
    // 新增：遍历参数类型中的维度表达式
    if (node->func_param.param_type &&
        node->func_param.param_type->kind == TYPE_ARRAY) {
      for (size_t i = 0; i < node->func_param.param_type->array.dim_count;
           ++i) {
        traverse_ast(node->func_param.param_type->array.dimensions[i].dim_expr,
                     pre_visit, post_visit, actx);
      }
    }
    break;
  case AST_VAR_DECL:
    if (node->var_decl.var_type &&
        node->var_decl.var_type->kind == TYPE_ARRAY) {
      for (size_t i = 0; i < node->var_decl.var_type->array.dim_count; ++i) {
        traverse_ast(node->var_decl.var_type->array.dimensions[i].dim_expr,
                     pre_visit, post_visit, actx);
      }
    }
    traverse_ast(node->var_decl.init_value, pre_visit, post_visit, actx);
    break;
  case AST_CONST_DECL:
    if (node->const_decl.const_type &&
        node->const_decl.const_type->kind == TYPE_ARRAY) {
      for (size_t i = 0; i < node->const_decl.const_type->array.dim_count;
           ++i) {
        traverse_ast(node->const_decl.const_type->array.dimensions[i].dim_expr,
                     pre_visit, post_visit, actx);
      }
    }
    traverse_ast(node->const_decl.value, pre_visit, post_visit, actx);
    break;
  case AST_IF_STMT:
    traverse_ast(node->if_stmt.cond, pre_visit, post_visit, actx);
    traverse_ast(node->if_stmt.then_stmt, pre_visit, post_visit, actx);
    traverse_ast(node->if_stmt.else_stmt, pre_visit, post_visit, actx);
    break;
  case AST_WHILE_STMT:
    traverse_ast(node->while_stmt.cond, pre_visit, post_visit, actx);
    traverse_ast(node->while_stmt.body, pre_visit, post_visit, actx);
    break;
  case AST_RETURN_STMT:
    traverse_ast(node->return_stmt.value, pre_visit, post_visit, actx);
    break;
  case AST_EXPR_STMT:
    traverse_ast(node->expr_stmt.expr, pre_visit, post_visit, actx);
    break;
  case AST_ASSIGN_STMT:
    traverse_ast(node->assign_stmt.lval, pre_visit, post_visit, actx);
    traverse_ast(node->assign_stmt.expr, pre_visit, post_visit, actx);
    break;
  case AST_BINARY_EXPR:
    traverse_ast(node->binary_expr.left, pre_visit, post_visit, actx);
    traverse_ast(node->binary_expr.right, pre_visit, post_visit, actx);
    break;
  case AST_UNARY_EXPR:
    traverse_ast(node->unary_expr.operand, pre_visit, post_visit, actx);
    break;
  case AST_CALL_EXPR:
    traverse_ast(node->call_expr.callee_expr, pre_visit, post_visit, actx);
    for (size_t i = 0; i < node->call_expr.arg_count; ++i) {
      traverse_ast(node->call_expr.args[i], pre_visit, post_visit, actx);
    }
    break;
  case AST_ARRAY_ACCESS:
    traverse_ast(node->array_access.array, pre_visit, post_visit, actx);
    traverse_ast(node->array_access.index, pre_visit, post_visit, actx);
    break;
  case AST_ARRAY_INIT:
    for (size_t i = 0; i < node->array_init.elem_count; ++i) {
      traverse_ast(node->array_init.elements[i], pre_visit, post_visit, actx);
    }
    break;
  default:
    break;
  }
  if (post_visit)
    post_visit(node, actx);
}

//==============================================================================
// 5. 增强的符号表构建 (第一遍) (Enhanced Symbol Table Construction - Pass 1)
//==============================================================================

// 标准库函数名列表，用于防止用户重定义
static const char *reserved_stdlib_names[] = {
    "getint", "getch",     "getfloat", "getarray", "getfarray",
    "putint", "putch",     "putfloat", "putarray", "putfarray",
    "putf",   "starttime", "stoptime", NULL};
// 检查一个名称是否是标准库函数名
static bool is_stdlib_name(const char *name) {
  for (int i = 0; reserved_stdlib_names[i]; ++i) {
    if (strcmp(name, reserved_stdlib_names[i]) == 0)
      return true;
  }
  return false;
}

// 在进入节点时构建符号
static void build_symbols_pre(ASTNode *node, AnalysisContext *actx) {
  if (!node)
    return;
  char msg_buffer[256];

  switch (node->node_type) {
  case AST_FUNC_DECL: {
    // 检查是否重定义标准库函数
    if (is_stdlib_name(node->func_decl.func_name)) {
      snprintf(msg_buffer, sizeof(msg_buffer),
               "Redefinition of standard library symbol '%s' is not allowed.",
               node->func_decl.func_name);
      add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                node->loc);
      actx->error_count++;
      node->sym = NULL;
      break;
    }
    // 检查在当前作用域中是否重定义
    Symbol *existing_sym =
        find_symbol_in_scope(actx->current_scope, node->func_decl.func_name);
    if (existing_sym) {
      snprintf(msg_buffer, sizeof(msg_buffer), "Redefinition of function '%s'.",
               node->func_decl.func_name);
      add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                node->loc);
      actx->error_count++;
      node->sym = existing_sym;
    } else {
      // 创建函数类型 - 优化：直接传递参数节点数组，避免中间内存分配
      Type *func_type = create_function_type_from_params(
          node->func_decl.return_type, node->func_decl.params,
          node->func_decl.param_count, false, actx->ast_ctx->pool);

      // 添加函数符号到当前作用域
      add_symbol(actx->current_scope, node->func_decl.func_name, func_type,
                 true, true, actx->ast_ctx);
      node->sym =
          find_symbol_in_scope(actx->current_scope, node->func_decl.func_name);
    }

    // 函数声明为其参数和函数体引入一个新的作用域
    SymbolTable *func_scope =
        create_symbol_table(actx->ast_ctx, actx->current_scope);
    node->func_decl.scope = func_scope;
    actx->current_scope = func_scope; // 进入新作用域

    // 将函数参数添加到函数作用域中
    for (size_t i = 0; i < node->func_decl.param_count; ++i) {
      ASTNode *param_node = node->func_decl.params[i];
      const char *param_name = param_node->func_param.name;
      Type *param_type = param_node->func_param.param_type;

      // 检查参数是否重名
      if (find_symbol_in_scope(actx->current_scope, param_name)) {
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Redefinition of parameter '%s'", param_name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                  param_node->loc);
        actx->error_count++;
      } else {
        add_symbol(actx->current_scope, param_name, param_type, false, false,
                   actx->ast_ctx);
        param_node->sym = find_symbol_in_scope(actx->current_scope, param_name);
      }
    }
    break;
  }
  case AST_COMPOUND_STMT: {
    // 复合语句（代码块）也引入新作用域，除非它是函数体的直接子节点
    // （因为函数声明已经创建了作用域）
    if (node->parent && node->parent->node_type != AST_FUNC_DECL) {
      SymbolTable *block_scope =
          create_symbol_table(actx->ast_ctx, actx->current_scope);
      node->compound_stmt.scope = block_scope;
      actx->current_scope = block_scope; // 进入新作用域
    } else {
      // 对于根复合语句或函数体，使用当前作用域
      node->compound_stmt.scope = actx->current_scope;
    }
    break;
  }
  case AST_VAR_DECL: {
    // 只处理全局变量
    if (actx->current_scope->parent == NULL) {
      if (is_stdlib_name(node->var_decl.name)) {
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Redefinition of standard library symbol '%s' is not allowed.",
                 node->var_decl.name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                  node->loc);
        actx->error_count++;
        node->sym = NULL;
        break;
      }
      // 检查在当前作用域中是否重定义
      Symbol *existing_sym =
          find_symbol_in_scope(actx->current_scope, node->var_decl.name);
      if (existing_sym) {
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Redefinition of global symbol '%s'.", node->var_decl.name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                  node->loc);
        actx->error_count++;
        node->sym = existing_sym;
      } else {
        Type *decl_type = node->var_decl.var_type;
        if (decl_type && decl_type->kind == TYPE_POINTER) {
          snprintf(msg_buffer, sizeof(msg_buffer),
                   "Pointer type variables are not allowed in SysY-2022");
          add_error(&actx->ast_ctx->errors, ERROR_TYPE_MISMATCH, msg_buffer,
                    node->loc);
        } else {
          add_symbol(actx->current_scope, node->var_decl.name,
                     node->var_decl.var_type, false, false, actx->ast_ctx);
          node->sym =
              find_symbol_in_scope(actx->current_scope, node->var_decl.name);
        }
      }
    }
    // 局部变量在第一遍完全忽略
    break;
  }
  case AST_CONST_DECL: {
    // 只处理全局常量
    if (actx->current_scope->parent == NULL) {
      if (is_stdlib_name(node->const_decl.name)) {
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Redefinition of standard library symbol '%s' is not allowed.",
                 node->const_decl.name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                  node->loc);
        actx->error_count++;
        node->sym = NULL;
        break;
      }
      // 检查在当前作用域中是否重定义
      Symbol *existing_sym =
          find_symbol_in_scope(actx->current_scope, node->const_decl.name);
      if (existing_sym) {
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Redefinition of global symbol '%s'.", node->const_decl.name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg_buffer,
                  node->loc);
        actx->error_count++;
        node->sym = existing_sym;
      } else {
        Type *decl_type = node->const_decl.const_type;
        if (decl_type && decl_type->kind == TYPE_POINTER) {
          snprintf(msg_buffer, sizeof(msg_buffer),
                   "Pointer type variables are not allowed in SysY-2022");
          add_error(&actx->ast_ctx->errors, ERROR_TYPE_MISMATCH, msg_buffer,
                    node->loc);
        } else {
          add_symbol(actx->current_scope, node->const_decl.name,
                     node->const_decl.const_type, false, true, actx->ast_ctx);
          node->sym =
              find_symbol_in_scope(actx->current_scope, node->const_decl.name);
        }
      }
    }
    // 局部常量在第一遍完全忽略
    break;
  }
  default:
    break;
  }
}

// 在离开节点时构建符号（主要用于退出作用域）
static void build_symbols_post(ASTNode *node, AnalysisContext *actx) {
  if (!node)
    return;
  // 只对称退出 pre_visit 创建新作用域的节点
  if (node->node_type == AST_FUNC_DECL ||
      (node->node_type == AST_COMPOUND_STMT && node->parent &&
       node->parent->node_type != AST_FUNC_DECL)) {
    if (actx->current_scope->parent) {
      actx->current_scope = actx->current_scope->parent;
    }
  }
}

//==============================================================================
// 6. 语义检查 (第二遍) (Semantic Checking - Pass 2)
//==============================================================================

// 在进入节点时进行语义检查
static void check_semantics_pre(ASTNode *node, AnalysisContext *actx) {
  if (!node)
    return;
  char msg[256];
  switch (node->node_type) {
  case AST_FUNC_DECL:
    actx->current_scope = node->func_decl.scope;
    actx->current_function_return_type = node->func_decl.return_type;
    actx->has_return_statement = false;
    // MODIFIED: Removed the check for parameter dimensions.
    // This logic has been moved to check_semantics_post for AST_FUNC_PARAM
    // to ensure dimension expressions are evaluated before being used.
    break;
  case AST_COMPOUND_STMT:
    if (node->compound_stmt.scope) {
      actx->current_scope = node->compound_stmt.scope;
    }
    break;
  case AST_WHILE_STMT:
    actx->loop_depth++;
    break;
  case AST_IDENTIFIER: {
    // 只查找符号，不在此处报告未定义错误
    node->sym = find_symbol(actx->current_scope, node->identifier.name);
    break;
  }
  // 关键：在这里添加局部符号
  case AST_VAR_DECL: {
    if (actx->current_scope->parent != NULL) { // 只处理局部变量
      Symbol *existing_sym =
          find_symbol_in_scope(actx->current_scope, node->var_decl.name);
      if (existing_sym) {
        snprintf(msg, sizeof(msg),
                 "Redefinition of symbol '%s' in the same scope.",
                 node->var_decl.name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg,
                  node->loc);
      } else {
        add_symbol(actx->current_scope, node->var_decl.name,
                   node->var_decl.var_type, false, false, actx->ast_ctx);
        node->sym =
            find_symbol_in_scope(actx->current_scope, node->var_decl.name);
      }
    }
    break;
  }
  case AST_CONST_DECL: {
    if (actx->current_scope->parent != NULL) { // 只处理局部常量
      Symbol *existing_sym =
          find_symbol_in_scope(actx->current_scope, node->const_decl.name);
      if (existing_sym) {
        snprintf(msg, sizeof(msg),
                 "Redefinition of symbol '%s' in the same scope.",
                 node->const_decl.name);
        add_error(&actx->ast_ctx->errors, ERROR_DUPLICATE_SYMBOL, msg,
                  node->loc);
      } else {
        add_symbol(actx->current_scope, node->const_decl.name,
                   node->const_decl.const_type, false, true, actx->ast_ctx);
        node->sym =
            find_symbol_in_scope(actx->current_scope, node->const_decl.name);
      }
    }
    break;
  }
  default:
    break;
  }
}

// 在离开节点时进行语义检查（后序遍历）
static void check_semantics_post(ASTNode *node, AnalysisContext *actx) {
  if (!node)
    return;
  ASTContext *ctx = actx->ast_ctx;
  char msg[256];
  switch (node->node_type) {
  case AST_CONSTANT:
    if (node->constant.type == CONST_INT) {
      node->eval_type = create_basic_type(BASIC_INT, true, ctx->pool);
    } else {
      node->eval_type = create_basic_type(BASIC_FLOAT, true, ctx->pool);
    }
    node->const_val = node->constant.value;
    node->is_lvalue = false;
    node->is_constant = true;
    break;
  case AST_STRING_LITERAL: {
    Type *char_type = create_basic_type(BASIC_I8, true, ctx->pool);
    node->eval_type = create_pointer_type(char_type, true, ctx->pool);
    node->is_lvalue = false;
    node->is_constant = true;
    break;
  }
  case AST_BINARY_EXPR: {
    Type *left_type = node->binary_expr.left->eval_type;
    Type *right_type = node->binary_expr.right->eval_type;
    if ((left_type && !is_numeric_type(left_type)) ||
        (right_type && !is_numeric_type(right_type))) {
      if (node->binary_expr.op != OP_AND && node->binary_expr.op != OP_OR) {
        add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                  "Operands of binary expression must be numeric", node->loc);
      }
    }
    if (node->binary_expr.op >= OP_EQ && node->binary_expr.op <= OP_OR) {
      node->eval_type = create_basic_type(BASIC_INT, false, ctx->pool);
    } else {
      if (left_type && right_type && left_type->kind == TYPE_BASIC &&
          right_type->kind == TYPE_BASIC &&
          (left_type->basic == BASIC_FLOAT ||
           right_type->basic == BASIC_FLOAT)) {
        node->eval_type = create_basic_type(BASIC_FLOAT, false, ctx->pool);
        if (node->binary_expr.op == OP_MOD) {
          add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                    "Invalid operands to binary % (have 'float')", node->loc);
        }
      } else {
        node->eval_type = create_basic_type(BASIC_INT, false, ctx->pool);
      }
    }
    node->is_lvalue = false;
    node->is_constant = is_constant_expression(node->binary_expr.left, actx) &&
                        is_constant_expression(node->binary_expr.right, actx);
    if (node->is_constant) {
      node->const_val = evaluate_const_expr_recursively(node, actx).value;
    }
    break;
  }
  case AST_UNARY_EXPR: {
    Type *op_type = node->unary_expr.operand->eval_type;
    if (op_type && !is_numeric_type(op_type)) {
      add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                "Operand of unary expression must be numeric", node->loc);
    }
    if (node->unary_expr.op == OP_NOT) {
      node->eval_type = create_basic_type(BASIC_INT, false, ctx->pool);
    } else {
      node->eval_type = op_type;
    }
    node->is_lvalue = false;
    node->is_constant = is_constant_expression(node->unary_expr.operand, actx);
    if (node->is_constant) {
      node->const_val = evaluate_const_expr_recursively(node, actx).value;
    }
    break;
  }
  case AST_IDENTIFIER:
    if (node->sym == NULL) {
      snprintf(msg, sizeof(msg), "Use of undeclared identifier '%s'",
               node->identifier.name);
      add_error(&ctx->errors, ERROR_UNDEFINED_VARIABLE, msg, node->loc);
      node->eval_type = create_basic_type(BASIC_INT, true,
                                          ctx->pool); // 假设为 int 以继续分析
      return;
    }
    node->eval_type = node->sym->type;
    node->is_lvalue = !node->sym->is_func && !node->sym->is_const;
    if (node->sym->is_const && node->sym->is_evaluated) {
      node->is_constant = true;
      node->const_val = node->sym->const_val;
    }
    break;
  case AST_ARRAY_ACCESS: {
    Type *array_type = node->array_access.array->eval_type;
    Type *index_type = node->array_access.index->eval_type;
    node->eval_type =
        create_basic_type(BASIC_INT, true, ctx->pool); // 默认错误类型
    node->is_lvalue = false;
    node->is_constant = false;
    if (index_type &&
        !is_type_compatible(create_basic_type(BASIC_INT, false, ctx->pool),
                            index_type, false)) {
      add_error(&ctx->errors, ERROR_INVALID_ARRAY_ACCESS,
                "Array subscript must be of integer type",
                node->array_access.index->loc);
    }
    if (array_type) {
      if (array_type->kind == TYPE_ARRAY) {
        Type *element_type = array_type->array.element_type;
        if (array_type->array.dim_count > 1) {
          node->eval_type = create_array_type(
              element_type, &array_type->array.dimensions[1],
              array_type->array.dim_count - 1, array_type->is_const, ctx->pool);
          node->is_lvalue = false;
        } else {
          node->eval_type = element_type;
          node->is_lvalue = !element_type->is_const;
        }
      } else {
        add_error(&ctx->errors, ERROR_INVALID_ARRAY_ACCESS,
                  "Subscripted value is not an array",
                  node->array_access.array->loc);
      }
    }
    break;
  }
  case AST_CALL_EXPR: {
    check_function_call(node, actx);
    break;
  }
  case AST_ASSIGN_STMT: {
    Type *lval_type =
        node->assign_stmt.lval ? node->assign_stmt.lval->eval_type : NULL;
    Type *expr_type =
        node->assign_stmt.expr ? node->assign_stmt.expr->eval_type : NULL;
    if (lval_type && expr_type) {
      if (!is_type_compatible(lval_type, expr_type, true)) {
        snprintf(msg, sizeof(msg),
                 "Type mismatch in assignment: cannot assign %s to %s",
                 expr_type->kind == TYPE_BASIC
                     ? (expr_type->basic == BASIC_INT ? "int" : "float")
                     : "non-basic",
                 lval_type->kind == TYPE_BASIC
                     ? (lval_type->basic == BASIC_INT ? "int" : "float")
                     : "non-basic");
        add_error(&ctx->errors, ERROR_TYPE_MISMATCH, msg, node->loc);
      }
    }
    if (node->assign_stmt.lval && node->assign_stmt.lval->sym &&
        node->assign_stmt.lval->sym->is_const) {
      add_error(&ctx->errors, ERROR_INVALID_ASSIGNMENT,
                "Cannot assign to const variable", node->assign_stmt.lval->loc);
    }
    if (node->assign_stmt.lval) {
      if (!node->assign_stmt.lval->is_lvalue) {
        add_error(
            &ctx->errors, ERROR_INVALID_ASSIGNMENT,
            "The left side of assignment is not a left value (not assignable)",
            node->assign_stmt.lval->loc);
      }
    }
    break;
  }
  case AST_RETURN_STMT: {
    Type *func_ret_type = actx->current_function_return_type;
    if (func_ret_type) {
      if (func_ret_type->kind == TYPE_VOID) {
        if (node->return_stmt.value) {
          add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                    "Void function cannot return a value", node->loc);
        }
      } else if (func_ret_type->kind == TYPE_BASIC &&
                 (func_ret_type->basic == BASIC_INT ||
                  func_ret_type->basic == BASIC_FLOAT)) {
        if (!node->return_stmt.value) {
          add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                    "Non-void function must return a value", node->loc);
        } else if (!is_type_compatible(func_ret_type,
                                       node->return_stmt.value->eval_type,
                                       true)) {
          add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                    "Return value type does not match function declaration",
                    node->loc);
        }
      }
    }
    // 标记本函数体出现过return
    actx->has_return_statement = true;
    break;
  }
  case AST_IF_STMT:
  case AST_WHILE_STMT: {
    ASTNode *cond = (node->node_type == AST_IF_STMT) ? node->if_stmt.cond
                                                     : node->while_stmt.cond;
    if (cond->eval_type && !is_numeric_type(cond->eval_type)) {
      add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                "Condition of if/while statement must be of a numeric type",
                cond->loc);
    }
    break;
  }
  case AST_BREAK_STMT: {
    if (actx->loop_depth == 0) {
      add_error(&ctx->errors, ERROR_BREAK_CONTINUE_OUTSIDE_LOOP,
                "break statement not within a loop", node->loc);
    }
    break;
  }
  case AST_CONTINUE_STMT: {
    if (actx->loop_depth == 0) {
      add_error(&ctx->errors, ERROR_BREAK_CONTINUE_OUTSIDE_LOOP,
                "continue statement not within a loop", node->loc);
    }
    break;
  }
  case AST_FUNC_DECL: {
    // 非void函数必须至少有一个return
    if (node->sym && node->sym->type->kind == TYPE_FUNCTION) {
      Type *ret_type = node->sym->type->function.return_type;
      if (ret_type && ret_type->kind != TYPE_VOID) {
        if (!actx->has_return_statement) {
          add_error(&ctx->errors, ERROR_MISSING_RETURN,
                    "Control may reach end of non-void function", node->loc);
        }
      }
    }
    actx->current_function_return_type = NULL;
    break;
  }
  // ADDED: New case for checking function parameter dimensions at the correct
  // time.
  case AST_FUNC_PARAM: {
    Type *param_type = node->func_param.param_type;
    if (param_type && param_type->kind == TYPE_ARRAY) {
      for (size_t j = 0; j < param_type->array.dim_count; ++j) {
        // According to SysY spec, the first dimension of a parameter is empty
        // `[]`, and subsequent dimensions must be constant expressions.
        bool is_first_dim = (j == 0);
        check_and_evaluate_dimension(
            param_type->array.dimensions[j].dim_expr,
            &param_type->array.dimensions[j].static_size, actx, is_first_dim);
      }
    }
    break;
  }
  case AST_VAR_DECL: {
    Type *decl_type = node->var_decl.var_type;
    if (decl_type && decl_type->kind == TYPE_ARRAY) {
      for (size_t i = 0; i < decl_type->array.dim_count; ++i) {
        // Variable declarations require all dimensions to be constant
        // expressions.
        check_and_evaluate_dimension(
            decl_type->array.dimensions[i].dim_expr,
            &decl_type->array.dimensions[i].static_size, actx, false);
      }
    }
    ASTNode *init_node = node->var_decl.init_value;
    if (init_node) {
      if (decl_type && decl_type->kind == TYPE_ARRAY) {
        check_array_initializer(init_node, decl_type, false, actx);
      } else if (decl_type && init_node->eval_type &&
                 !is_type_compatible(decl_type, init_node->eval_type, true)) {
        add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                  "Incompatible type for initializer", node->loc);
      }
    }
    break;
  }
  case AST_CONST_DECL: {
    // 局部常量求值：仅当在局部作用域时才调用辅助函数 (全局常量已在 Pass 1.5
    // 中处理)
    if (actx->current_scope->parent != NULL) {
      evaluate_single_const_decl(node, actx);
    }
    // 数组常量初始化器的额外检查
    Type *decl_type = node->const_decl.const_type;
    ASTNode *init_node = node->const_decl.value;
    if (init_node && decl_type && decl_type->kind == TYPE_ARRAY) {
      check_array_initializer(init_node, decl_type, true, actx);
    }
    break;
  }
  default:
    break;
  }
  // 统一作用域退出和循环深度管理
  switch (node->node_type) {
  case AST_FUNC_DECL:
    if (actx->current_scope->parent) {
      actx->current_scope = actx->current_scope->parent;
    }
    break;
  case AST_COMPOUND_STMT:
    if (node->parent && node->parent->node_type != AST_FUNC_DECL) {
      if (actx->current_scope->parent) {
        actx->current_scope = actx->current_scope->parent;
      }
    }
    break;
  case AST_WHILE_STMT:
    actx->loop_depth--;
    break;
  default:
    break;
  }
}

//==============================================================================
// 7. 类型系统辅助函数 (Type System Helper Functions)
//==============================================================================

// 检查类型是否为数值类型（int 或 float）
static bool is_numeric_type(Type *type) {
  if (!type || type->kind != TYPE_BASIC)
    return false;
  return type->basic == BASIC_INT || type->basic == BASIC_FLOAT ||
         type->basic == BASIC_DOUBLE || type->basic == BASIC_I8;
}

// 替换你的 is_type_same 函数
static bool is_type_same(Type *t1, Type *t2, bool ignore_top_level_const) {
  if (!t1 || !t2)
    return t1 == t2;
  if (t1->kind != t2->kind)
    return false;
  if (!ignore_top_level_const && t1->is_const != t2->is_const)
    return false;

  switch (t1->kind) {
  case TYPE_VOID:
    return true;
  case TYPE_BASIC:
    return t1->basic == t2->basic;
  case TYPE_POINTER:
    return is_type_same(t1->pointer.element_type, t2->pointer.element_type,
                        false);
  case TYPE_ARRAY:
    if (t1->array.dim_count != t2->array.dim_count)
      return false;
    if (!is_type_same(t1->array.element_type, t2->array.element_type, false))
      return false;

    for (size_t i = 0; i < t1->array.dim_count; i++) {
      ArrayDimension d1 = t1->array.dimensions[i];
      ArrayDimension d2 = t2->array.dimensions[i];

      // 两个维度必须具有相同的动态/静态属性
      if (d1.is_dynamic != d2.is_dynamic) {
        return false;
      }

      // 如果它们是静态的，大小必须相等
      if (!d1.is_dynamic && d1.static_size != d2.static_size) {
        return false;
      }
    }
    return true;

  case TYPE_FUNCTION:
    if (!is_type_same(t1->function.return_type, t2->function.return_type,
                      false))
      return false;
    if (t1->function.param_count != t2->function.param_count)
      return false;
    if (t1->function.is_variadic != t2->function.is_variadic)
      return false;
    for (size_t i = 0; i < t1->function.param_count; ++i) {
      if (!is_type_same(t1->function.param_types[i],
                        t2->function.param_types[i], false)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

// MODIFIED: Added a special rule for array compatibility to handle passing
// multi-dimensional arrays to functions expecting 1D arrays (like getfarray).
static bool is_type_compatible(Type *target, Type *source,
                               bool allow_numeric_conversion) {
  if (!target || !source)
    return false;

  // Rule 1: Types are exactly the same (ignoring top-level const for
  // assignments)
  if (is_type_same(target, source, true)) {
    return true;
  }

  // Rule 2: Implicit conversion between numeric types
  if (target->kind == TYPE_BASIC && source->kind == TYPE_BASIC) {
    if (allow_numeric_conversion) {
      // SysY allows implicit conversion between int and float
      return is_numeric_type(target) && is_numeric_type(source);
    } else {
      // Strict check, no conversion allowed
      return target->basic == source->basic;
    }
  }

  // Rule 3: Array compatibility
  if (target->kind == TYPE_ARRAY && source->kind == TYPE_ARRAY) {
    // 3.1 Base element types must be compatible (usually means identical)
    if (!is_type_same(get_array_base_element_type(target),
                      get_array_base_element_type(source), true)) {
      return false;
    }

    // 3.2 SPECIAL SYSY RULE: If the target (parameter) is a 1D array,
    // it can accept a source (argument) of any matching base type, regardless
    // of the source's dimensions. This is for functions like
    // getfarray/putfarray that treat memory as a flat block.
    if (target->array.dim_count == 1) {
      return true;
    }

    // 3.3 STANDARD RULE: For other cases (e.g., multi-dim to multi-dim),
    // the dimension count must be the same.
    if (target->array.dim_count != source->array.dim_count) {
      return false;
    }

    // 3.4 Dimensions from the second one onwards must match in size.
    for (size_t i = 1; i < target->array.dim_count; ++i) {
      if (target->array.dimensions[i].static_size !=
          source->array.dimensions[i].static_size) {
        return false;
      }
    }
    return true;
  }

  // Rule 4: Array-to-pointer decay (not common in SysY-2022, but good practice)
  if (target->kind == TYPE_POINTER && source->kind == TYPE_ARRAY) {
    return is_type_same(target->pointer.element_type,
                        source->array.element_type, true);
  }

  return false;
}

// 添加预定义的库函数到全局作用域
static void add_predefined_symbols(AnalysisContext *actx) {
  ASTContext *ctx = actx->ast_ctx;
  MemoryPool *pool = ctx->pool;
  Type *type_int = create_basic_type(BASIC_INT, false, pool);
  Type *type_float = create_basic_type(BASIC_FLOAT, false, pool);
  Type *type_void = create_void_type(pool);
  // 数组参数类型
  // **重要修改：将 dyn_dim 从栈上分配改为从内存池分配**
  ArrayDimension *dyn_dim_ptr = pool_alloc(pool, sizeof(ArrayDimension));
  dyn_dim_ptr->is_dynamic = true;
  dyn_dim_ptr->static_size = -1;
  dyn_dim_ptr->dim_expr = NULL;

  Type *type_int_array_param =
      create_array_type(type_int, dyn_dim_ptr, 1, false, pool);
  Type *type_float_array_param =
      create_array_type(type_float, dyn_dim_ptr, 1, false, pool);
  // getint, getch, getfloat
  add_symbol(actx->current_scope, "getint",
             create_function_type(type_int, NULL, 0, false, pool), true, true,
             ctx);
  add_symbol(actx->current_scope, "getch",
             create_function_type(type_int, NULL, 0, false, pool), true, true,
             ctx);
  add_symbol(actx->current_scope, "getfloat",
             create_function_type(type_float, NULL, 0, false, pool), true, true,
             ctx);
  // getarray, getfarray
  Type *getarray_params[] = {type_int_array_param};
  add_symbol(actx->current_scope, "getarray",
             create_function_type(type_int, getarray_params, 1, false, pool),
             true, true, ctx);
  Type *getfarray_params[] = {type_float_array_param};
  add_symbol(actx->current_scope, "getfarray",
             create_function_type(type_int, getfarray_params, 1, false, pool),
             true, true, ctx);
  // putint, putch, putfloat
  Type *putint_params[] = {type_int};
  add_symbol(actx->current_scope, "putint",
             create_function_type(type_void, putint_params, 1, false, pool),
             true, true, ctx);
  Type *putch_params[] = {type_int};
  add_symbol(actx->current_scope, "putch",
             create_function_type(type_void, putch_params, 1, false, pool),
             true, true, ctx);
  Type *putfloat_params[] = {type_float};
  add_symbol(actx->current_scope, "putfloat",
             create_function_type(type_void, putfloat_params, 1, false, pool),
             true, true, ctx);
  // putarray, putfarray
  Type *putarray_params[] = {type_int, type_int_array_param};
  add_symbol(actx->current_scope, "putarray",
             create_function_type(type_void, putarray_params, 2, false, pool),
             true, true, ctx);
  Type *putfarray_params[] = {type_int, type_float_array_param};
  add_symbol(actx->current_scope, "putfarray",
             create_function_type(type_void, putfarray_params, 2, false, pool),
             true, true, ctx);
  // putf
  Type *type_const_char_ptr_param =
      create_pointer_type(create_basic_type(BASIC_I8, true, pool), false, pool);
  Type *putf_params[] = {type_const_char_ptr_param};
  add_symbol(actx->current_scope, "putf",
             create_function_type(type_void, putf_params, 1, true, pool), true,
             true, ctx);
  // starttime, stoptime
  add_symbol(actx->current_scope, "starttime",
             create_function_type(type_void, NULL, 0, false, pool), true, true,
             ctx);
  add_symbol(actx->current_scope, "stoptime",
             create_function_type(type_void, NULL, 0, false, pool), true, true,
             ctx);
}

//==============================================================================
// 8. 常量表达式求值 (Constant Expression Evaluation)
//==============================================================================

// 检查一个节点是否代表一个常量表达式
static bool is_constant_expression(ASTNode *node, AnalysisContext *actx) {
  if (!node)
    return false;

  if (node->node_type == AST_CONSTANT)
    return true;
  if (node->is_constant)
    return true;

  // 常量标识符
  if (node->node_type == AST_IDENTIFIER && node->sym && node->sym->is_const) {
    // 当前在常量表达式上下文中，允许使用任何const变量
    if (actx->is_in_constant_context) {
      return true; // 在常量表达式上下文中允许使用const变量
    }
    // 否则只允许已求值的常量
    return node->sym->is_evaluated;
  }

  // 操作数是常量的单目表达式
  if (node->node_type == AST_UNARY_EXPR) {
    return is_constant_expression(node->unary_expr.operand, actx);
  }

  // 操作数都是常量的双目表达式
  if (node->node_type == AST_BINARY_EXPR) {
    return is_constant_expression(node->binary_expr.left, actx) &&
           is_constant_expression(node->binary_expr.right, actx);
  }

  // 新增：处理数组初始化列表
  if (node->node_type == AST_ARRAY_INIT) {
    for (size_t i = 0; i < node->array_init.elem_count; ++i) {
      if (!is_constant_expression(node->array_init.elements[i], actx)) {
        return false;
      }
    }
    return true;
  }

  return false;
}

// 递归地对常量表达式进行求值
static EvaluatedConst evaluate_const_expr_recursively(ASTNode *node,
                                                      AnalysisContext *actx) {
  EvaluatedConst result = {.is_const = false, .is_valid = true};
  if (!node)
    return result;

  switch (node->node_type) {
  case AST_CONSTANT:
    result.is_const = true;
    result.value = node->constant.value;
    // 新增：直接在这里设置节点的 eval_type，以供上层使用
    if (node->constant.type == CONST_INT) {
      node->eval_type = create_basic_type(BASIC_INT, true, actx->ast_ctx->pool);
    } else {
      node->eval_type =
          create_basic_type(BASIC_FLOAT, true, actx->ast_ctx->pool);
    }
    break;
  case AST_IDENTIFIER:;
    Symbol *identified_sym =
        find_symbol(actx->current_scope, node->identifier.name);
    if (identified_sym && identified_sym->is_const &&
        identified_sym->is_evaluated) {
      result.is_const = true;
      result.value = identified_sym->const_val;
      node->eval_type =
          identified_sym->type; // 设置 eval_type (因为这里是常量，类型已知)
    } else {
      result.is_const = false;
      // 注意：如果符号未找到或不是常量，这里不报告错误。
      // 错误会在 Pass 2 的 check_semantics_post 中统一处理。
    }
    break;
  case AST_UNARY_EXPR: {
    EvaluatedConst operand_val =
        evaluate_const_expr_recursively(node->unary_expr.operand, actx);
    if (!operand_val.is_const)
      break;
    // 自己推断类型，不再读取 operand->eval_type
    bool op_is_int = node->unary_expr.operand->eval_type &&
                     node->unary_expr.operand->eval_type->kind == TYPE_BASIC &&
                     node->unary_expr.operand->eval_type->basic == BASIC_INT;
    result.is_const = true;
    if (node->unary_expr.op == OP_NOT) {
      float val = op_is_int ? (float)operand_val.value.int_val
                            : operand_val.value.float_val;
      result.value.int_val = (val == 0.0f);
      node->eval_type = create_basic_type(BASIC_INT, true, actx->ast_ctx->pool);
    } else if (op_is_int) {
      result.value.int_val = (node->unary_expr.op == OP_NEG)
                                 ? -operand_val.value.int_val
                                 : operand_val.value.int_val;
      node->eval_type = create_basic_type(BASIC_INT, true, actx->ast_ctx->pool);
    } else {
      result.value.float_val = (node->unary_expr.op == OP_NEG)
                                   ? -operand_val.value.float_val
                                   : operand_val.value.float_val;
      node->eval_type =
          create_basic_type(BASIC_FLOAT, true, actx->ast_ctx->pool);
    }
    break;
  }
  case AST_BINARY_EXPR: {
    EvaluatedConst left_val =
        evaluate_const_expr_recursively(node->binary_expr.left, actx);
    EvaluatedConst right_val =
        evaluate_const_expr_recursively(node->binary_expr.right, actx);
    if (!left_val.is_const || !right_val.is_const)
      break;
    // 自己推断类型，不再读取子节点的 eval_type
    bool left_is_int = node->binary_expr.left->eval_type &&
                       node->binary_expr.left->eval_type->kind == TYPE_BASIC &&
                       node->binary_expr.left->eval_type->basic == BASIC_INT;
    bool right_is_int =
        node->binary_expr.right->eval_type &&
        node->binary_expr.right->eval_type->kind == TYPE_BASIC &&
        node->binary_expr.right->eval_type->basic == BASIC_INT;
    result.is_const = true;
    bool is_float_op = !left_is_int || !right_is_int;
    if (is_float_op) {
      float lv = left_is_int ? (float)left_val.value.int_val
                             : left_val.value.float_val;
      float rv = right_is_int ? (float)right_val.value.int_val
                              : right_val.value.float_val;
      switch (node->binary_expr.op) {
      case OP_ADD:
        result.value.float_val = lv + rv;
        break;
      case OP_SUB:
        result.value.float_val = lv - rv;
        break;
      case OP_MUL:
        result.value.float_val = lv * rv;
        break;
      case OP_DIV:
        if (rv != 0.0f)
          result.value.float_val = lv / rv;
        else {
          result.is_const = false;
          add_error(&actx->ast_ctx->errors, ERROR_RUNTIME,
                    "Division by zero in const expr", node->loc);
        }
        break;
      case OP_LT:
        result.value.int_val = lv < rv;
        break;
      case OP_GT:
        result.value.int_val = lv > rv;
        break;
      case OP_LE:
        result.value.int_val = lv <= rv;
        break;
      case OP_GE:
        result.value.int_val = lv >= rv;
        break;
      case OP_EQ:
        result.value.int_val = lv == rv;
        break;
      case OP_NE:
        result.value.int_val = lv != rv;
        break;
      case OP_AND:
        result.value.int_val = (lv != 0.0f) && (rv != 0.0f);
        break;
      case OP_OR:
        result.value.int_val = (lv != 0.0f) || (rv != 0.0f);
        break;
      default:
        result.is_const = false;
        break;
      }
      if (node->binary_expr.op >= OP_LT && node->binary_expr.op <= OP_OR) {
        node->eval_type =
            create_basic_type(BASIC_INT, true, actx->ast_ctx->pool);
      } else {
        node->eval_type =
            create_basic_type(BASIC_FLOAT, true, actx->ast_ctx->pool);
      }
    } else { // 整数运算
      int lv = left_val.value.int_val;
      int rv = right_val.value.int_val;
      switch (node->binary_expr.op) {
      case OP_ADD:
        result.value.int_val = lv + rv;
        break;
      case OP_SUB:
        result.value.int_val = lv - rv;
        break;
      case OP_MUL:
        result.value.int_val = lv * rv;
        break;
      case OP_DIV:
        if (rv != 0)
          result.value.int_val = lv / rv;
        else {
          result.is_const = false;
          add_error(&actx->ast_ctx->errors, ERROR_RUNTIME,
                    "Division by zero in const expr", node->loc);
        }
        break;
      case OP_MOD:
        if (rv != 0)
          result.value.int_val = lv % rv;
        else {
          result.is_const = false;
          add_error(&actx->ast_ctx->errors, ERROR_RUNTIME,
                    "Modulo by zero in const expr", node->loc);
        }
        break;
      case OP_LT:
        result.value.int_val = lv < rv;
        break;
      case OP_GT:
        result.value.int_val = lv > rv;
        break;
      case OP_LE:
        result.value.int_val = lv <= rv;
        break;
      case OP_GE:
        result.value.int_val = lv >= rv;
        break;
      case OP_EQ:
        result.value.int_val = lv == rv;
        break;
      case OP_NE:
        result.value.int_val = lv != rv;
        break;
      case OP_AND:
        result.value.int_val = lv && rv;
        break;
      case OP_OR:
        result.value.int_val = lv || rv;
        break;
      default:
        result.is_const = false;
        break;
      }
      node->eval_type = create_basic_type(BASIC_INT, true, actx->ast_ctx->pool);
    }
    break;
  }
  default:
    break;
  }
  return result;
}

//==============================================================================
// 9. 语义检查辅助函数 (Semantic Check Helpers)
//==============================================================================

// 1. 重新定义 ControlFlowResult 结构体，使其更精确地描述路径状态
//    放在文件顶部的 "内部类型定义和分析上下文" 部分
// 2. 声明新的递归辅助函数
static void check_array_initializer_recursive(InitContext *init_ctx,
                                              Type *target_type,
                                              bool is_const_context,
                                              AnalysisContext *actx);

// 3. 实现新的入口函数和递归函数
//    请用这两个新函数完全替换掉你旧的 check_array_initializer

/**
 * @brief 数组初始化检查的入口函数。
 * @details 设置初始化上下文，并调用递归辅助函数开始检查。
 */
static void check_array_initializer(ASTNode *init_node, Type *target_type,
                                    bool is_const_context,
                                    AnalysisContext *actx) {
  if (!init_node || !target_type)
    return;

  // --- 目标是标量 ---
  if (target_type->kind != TYPE_ARRAY) {
    if (init_node->node_type == AST_ARRAY_INIT) {
      add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
                "Braces around scalar initializer", init_node->loc);
    } else if (init_node->eval_type &&
               !is_type_compatible(target_type, init_node->eval_type, true)) {
      add_error(&actx->ast_ctx->errors, ERROR_TYPE_MISMATCH,
                "Scalar initializer type mismatch", init_node->loc);
    } else if (is_const_context && !is_constant_expression(init_node, actx)) {
      add_error(&actx->ast_ctx->errors, ERROR_INVALID_CONVERSION,
                "Initializer for const variable must be constant",
                init_node->loc);
    }
    return;
  }

  // --- 目标是数组 ---
  if (init_node->node_type != AST_ARRAY_INIT) {
    add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
              "Array requires braced initializer list", init_node->loc);
    return;
  }

  InitContext init_ctx = {.init_list_node = init_node, .current_idx = 0};
  check_array_initializer_recursive(&init_ctx, target_type, is_const_context,
                                    actx);

  // 检查是否有过多的初始化元素
  if (init_ctx.current_idx < (int)init_node->array_init.elem_count) {
    // 这意味着顶层列表中的某些元素没有被完全消耗掉
    add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
              "Too many elements in array initializer",
              init_node->array_init.elements[init_ctx.current_idx]->loc);
  }
}

/**
 * @brief 递归地检查数组初始化列表。
 * @details 这个函数模拟C编译器的行为，可以处理嵌套和扁平化的初始化。
 */
static void check_array_initializer_recursive(InitContext *init_ctx,
                                              Type *target_type,
                                              bool is_const_context,
                                              AnalysisContext *actx) {
  // 基本情况: 目标是标量 (非数组)
  if (target_type->kind != TYPE_ARRAY) {
    if (init_ctx->current_idx >=
        (int)init_ctx->init_list_node->array_init.elem_count) {
      return; // 初始化列表用尽, 合法
    }
    ASTNode *current_elem =
        init_ctx->init_list_node->array_init.elements[init_ctx->current_idx];

    if (current_elem->node_type == AST_ARRAY_INIT) {
      // 一个标量不能被一个列表初始化，但有一个例外: char a = {"a"};
      // SysY 不支持，所以这是个错误。
      add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
                "Braces around scalar initializer", current_elem->loc);
    } else {
      check_array_initializer(current_elem, target_type, is_const_context,
                              actx);
    }
    init_ctx->current_idx++;
    return;
  }

  // --- 递归情况: 目标是数组 ---
  size_t dim_size = target_type->array.dimensions[0].static_size;
  if (dim_size == 0)
    return; // 零长度数组

  Type *sub_type =
      (target_type->array.dim_count > 1)
          ? create_array_type(target_type->array.element_type,
                              target_type->array.dimensions + 1,
                              target_type->array.dim_count - 1,
                              target_type->is_const, actx->ast_ctx->pool)
          : target_type->array.element_type;

  // 循环遍历当前维度的所有元素
  for (size_t i = 0; i < dim_size; ++i) {
    if (init_ctx->current_idx >=
        (int)init_ctx->init_list_node->array_init.elem_count) {
      return; // 初始化列表用尽，剩余的将被隐式初始化
    }

    ASTNode *current_elem =
        init_ctx->init_list_node->array_init.elements[init_ctx->current_idx];

    // 检查当前初始化器元素是否是匹配的子列表
    if (current_elem->node_type == AST_ARRAY_INIT) {
      init_ctx->current_idx++; // 消耗掉这个子列表 `"{...}"`

      InitContext sub_ctx = {.init_list_node = current_elem, .current_idx = 0};
      check_array_initializer_recursive(&sub_ctx, sub_type, is_const_context,
                                        actx);

      if (sub_ctx.current_idx < (int)current_elem->array_init.elem_count) {
        add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
                  "Too many elements in sub-initializer",
                  current_elem->array_init.elements[sub_ctx.current_idx]->loc);
      }
    } else {
      // 扁平化初始化：用当前元素初始化子类型的第一个标量
      check_array_initializer_recursive(init_ctx, sub_type, is_const_context,
                                        actx);
    }
  }
}

// MODIFIED: Function updated to reflect new signature and logic.
static bool check_and_evaluate_dimension(ASTNode *dim_expr, int *evaluated_size,
                                         AnalysisContext *actx,
                                         bool is_func_param_first_dim) {
  // Case 1: Dimension expression is empty (e.g., `int a[]`)
  if (!dim_expr) {
    *evaluated_size = -1; // Mark as unknown/dynamic
    // This is only allowed for the first dimension of a function parameter
    if (is_func_param_first_dim) {
      return true;
    }
    // Or if size can be inferred from an initializer (not handled here, but a
    // valid C feature)
    add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
              "Array dimension size must be specified",
              (dim_expr ? dim_expr->loc : (SourceLocation){0, 0, 0, 0}));
    return false;
  }

  // Case 2: It's the first dimension of a function parameter.
  // It doesn't need to be a constant. We just mark its size as dynamic and move
  // on.
  if (is_func_param_first_dim) {
    *evaluated_size = -1;
    return true;
  }

  // Case 3: All other dimensions must be a constant expression.
  actx->is_in_constant_context = true;
  bool is_const = is_constant_expression(dim_expr, actx);
  actx->is_in_constant_context = false;

  if (!is_const) {
    add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
              "Array dimension must be a constant expression", dim_expr->loc);
    *evaluated_size = -1;
    return false;
  }

  // Evaluate the constant expression.
  EvaluatedConst eval_result = evaluate_const_expr_recursively(dim_expr, actx);
  if (!eval_result.is_const || !eval_result.is_valid) {
    *evaluated_size = -1;
    // Error is already reported by the recursive call
    return false;
  }

  // Check the type and value of the result.
  int size;
  if (dim_expr->eval_type && dim_expr->eval_type->kind == TYPE_BASIC &&
      dim_expr->eval_type->basic == BASIC_INT) {
    size = eval_result.value.int_val;
  } else {
    add_error(&actx->ast_ctx->errors, ERROR_TYPE_MISMATCH,
              "Array dimension must be of integer type", dim_expr->loc);
    *evaluated_size = -1;
    return false;
  }

  if (size <= 0) {
    add_error(&actx->ast_ctx->errors, ERROR_INVALID_ARRAY_INIT,
              "Array dimension must be a positive integer", dim_expr->loc);
    *evaluated_size = -1;
    return false;
  }

  *evaluated_size = size;
  return true;
}

// MODIFIED: This function is simplified to rely on the corrected
// `is_type_compatible`.
static bool check_function_call(ASTNode *call_node, AnalysisContext *actx) {
  if (!call_node || call_node->node_type != AST_CALL_EXPR)
    return false;
  ASTContext *ctx = actx->ast_ctx;
  ASTNode *callee = call_node->call_expr.callee_expr;
  if (!callee)
    return false;
  Type *callee_type = callee->eval_type;
  if (!callee_type || callee_type->kind != TYPE_FUNCTION) {
    add_error(&ctx->errors, ERROR_INVALID_PARAMETER, "Callee is not a function",
              call_node->loc);
    call_node->eval_type = create_basic_type(BASIC_INT, false, ctx->pool);
    return false;
  }
  // 1. putf special check: first argument must be a string literal
  if (callee->node_type == AST_IDENTIFIER &&
      strcmp(callee->identifier.name, "putf") == 0) {
    if (call_node->call_expr.arg_count < 1 || !call_node->call_expr.args[0] ||
        call_node->call_expr.args[0]->node_type != AST_STRING_LITERAL) {
      add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                "The first argument to putf must be a string literal",
                call_node->loc);
    }
  }

  // 2. Check argument count
  size_t expected = callee_type->function.param_count;
  size_t actual = call_node->call_expr.arg_count;
  bool variadic = callee_type->function.is_variadic;
  if ((!variadic && actual != expected) || (variadic && actual < expected)) {
    add_error(&ctx->errors, ERROR_INVALID_PARAMETER,
              "Incorrect number of arguments in function call", call_node->loc);
    call_node->eval_type = callee_type->function.return_type;
    return false;
  }

  // 3. Check each argument's type
  for (size_t i = 0; i < expected; ++i) {
    ASTNode *arg = call_node->call_expr.args[i];
    Type *param_type = callee_type->function.param_types[i];

    if (!arg || !arg->eval_type) {
      add_error(&ctx->errors, ERROR_TYPE_MISMATCH, "Invalid function argument",
                arg ? arg->loc : call_node->loc);
      continue;
    }

    // Unified type compatibility check for all types, including arrays.
    if (!is_type_compatible(param_type, arg->eval_type, true)) {
      add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                "Function argument type mismatch", arg->loc);
    }
  }

  // 4. Set return type
  call_node->eval_type = callee_type->function.return_type;
  return true;
}

/**
 * @brief 辅助函数：求值单个常量声明。
 * @details 这个函数封装了常量求值的核心逻辑，供不同阶段调用。
 *          它会设置符号的 const_val 和 is_evaluated 标志。
 */
static void evaluate_single_const_decl(ASTNode *node, AnalysisContext *actx) {
  if (!node || node->node_type != AST_CONST_DECL)
    return;

  ASTContext *ctx = actx->ast_ctx;
  Type *decl_type = node->const_decl.const_type;
  ASTNode *init_node = node->const_decl.value;

  if (!init_node) {
    add_error(&ctx->errors, ERROR_MISSING_INITIALIZER,
              "const declaration must be initialized", node->loc);
  } else if (node->sym) {
    if (decl_type && decl_type->kind == TYPE_ARRAY) {
      // 数组常量：检查所有维度表达式
      for (size_t i = 0; i < decl_type->array.dim_count; ++i) {
        check_and_evaluate_dimension(
            decl_type->array.dimensions[i].dim_expr,
            &decl_type->array.dimensions[i].static_size, actx, false);
      }
      node->sym->is_evaluated = true;
    } else { // 标量常量
      // 直接尝试求值，而不是先用 is_constant_expression 预判
      EvaluatedConst const_eval =
          evaluate_const_expr_recursively(init_node, actx);
      // 这里只需求值并设置符号，不涉及数组维度判断
      if (const_eval.is_const && const_eval.is_valid) {
        node->sym->const_val = const_eval.value;
        node->sym->is_evaluated = true;
        // Also check if initializer type is compatible with declaration type
        if (!is_type_compatible(decl_type, init_node->eval_type, true)) {
          add_error(&ctx->errors, ERROR_TYPE_MISMATCH,
                    "Incompatible types in const initialization", node->loc);
        }
      } else {
        add_error(&ctx->errors, ERROR_INVALID_CONVERSION,
                  "Initializer for const is not a constant expression",
                  node->loc);
      }
    }
  }
}