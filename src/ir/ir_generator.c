/**
 * @file ir_generator.c
 * @brief 使用 IRBuilder 从 AST 实现优化的 IR 生成。
 * @details
 * - 优化的内存管理和错误处理
 * - 更好的类型转换和常量折叠
 * - 更清晰的代码结构和职责分离
 * - 增强了对逻辑运算符的短路求值
 * - 更好地支持复杂表达式和控制流
 */
#include "ir/ir_generator.h"
#include "ast.h"
#include "error.h" // for add_error, ERROR_SEMANTIC
#include "ir/ir.h" // for destroy_ir_module
#include "ir/ir_builder.h"
#include "ir/ir_data_structures.h"
#include "ir/ir_utils.h"
#include "location.h" // for SourceLocation
#include "logger.h"
#include "symbol_table.h"
#include <assert.h>
#include <stdbool.h> // for false, true, bool
#include <stdio.h>
#include <string.h>

// --- IR 生成上下文的类型定义 ---

/**
 * @struct StringLiteralEntry
 * @brief 用于管理字符串字面量的条目，避免在IR中生成重复的全局字符串。
 */
typedef struct StringLiteralEntry {
  const char *ast_value;           ///< AST中原始的字符串值
  IRValue *global_var;             ///< 指向IR中对应的全局字符串变量
  struct StringLiteralEntry *next; ///< 指向下一个条目的链表指针
} StringLiteralEntry;

/**
 * @struct IRGenContext
 * @brief 在IR生成期间维护所有状态的上下文结构。
 */
typedef struct IRGenContext {
  ASTContext *ast_ctx;        ///< 指向前端AST上下文
  IRModule *module;           ///< 当前正在构建的IR模块
  IRBuilder builder;          ///< 用于流式构建IR指令的构建器
  SymbolTable *current_scope; ///< 当前正在访问的AST作用域
  int str_lit_count;          ///< 用于生成唯一字符串字面量名称的计数器
  IRBasicBlock *loop_cond_bb; ///< 当前循环的条件块（用于continue）
  IRBasicBlock *loop_exit_bb; ///< 当前循环的退出块（用于break）
  ValueMap value_map; ///< 核心数据结构：映射AST符号到其在IR中的地址（IRValue*）
  StringLiteralEntry *string_literals; ///< 字符串字面量缓存列表
  int error_count;                     ///< 生成过程中的错误计数
  int warning_count;                   ///< 生成过程中的警告计数
} IRGenContext;

// --- 静态函数前向声明 ---
static void generate_globals(IRGenContext *ctx, ASTNode *root);
static IRValue *generate_constant_initializer(IRGenContext *ctx, Type *type,
                                              ASTNode *init_node);
static void generate_function(IRGenContext *ctx, ASTNode *func_decl_node);
static void generate_statement(IRGenContext *ctx, ASTNode *stmt_node);
static IRValue *generate_expression(IRGenContext *ctx, ASTNode *expr_node,
                                    bool want_address);
static void find_and_alloc_locals_visitor(ASTNode *node, void *user_data);
static void prescan_string_literals_visitor(ASTNode *node, void *user_data);
static void simple_ast_traverse(ASTNode *node,
                                void (*visitor)(ASTNode *, void *),
                                void *user_data);
static void generate_local_array_init(IRGenContext *ctx, IRValue *array_addr,
                                      Type *array_type, ASTNode *init_list);
static void init_value_map(IRGenContext *ctx);
static void map_addr(IRGenContext *ctx, Symbol *sym, IRValue *addr);
static IRValue *find_addr(IRGenContext *ctx, Symbol *sym);
static Symbol *find_symbol_for_addr(IRGenContext *ctx, IRValue *addr);
static Type *get_ir_pointer_type(IRGenContext *ctx, Type *base_type);
static IRValue *create_type_conversion(IRGenContext *ctx, IRValue *src_val,
                                       Type *src_type, Type *dest_type);
static const char *get_icmp_cond(OperatorType op);
static const char *get_fcmp_cond(OperatorType op);
static void report_generation_error(IRGenContext *ctx, const char *message,
                                    SourceLocation loc);

static Opcode operator_type_to_ir_opcode(OperatorType op, bool is_float);
IRModule *create_ir_module(const char *filename, LogConfig *log_config);

// --- 主驱动函数 ---

/**
 * @brief 从 AST 生成内存中的 IR 模块。
 * @param ast_ctx 包含完整前端信息的上下文。
 * @return 成功则返回指向新创建的 IRModule 的指针，否则返回 NULL。
 */
IRModule *generate_ir_module(ASTContext *ast_ctx) {
  LOG_INFO(&ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
           "Starting optimized IR generation...");

  if (!ast_ctx || !ast_ctx->root) {
    LOG_ERROR(&ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
              "Invalid AST context or root node");
    return NULL;
  }

  // 创建顶层 IR 模块，传递日志配置
  IRModule *module =
      create_ir_module(ast_ctx->source_filename, &ast_ctx->log_config);
  if (!module) {
    LOG_ERROR(&ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
              "Failed to create IR module");
    return NULL;
  }

  // 初始化 IR 生成上下文
  IRGenContext ctx = {.ast_ctx = ast_ctx,
                      .module = module,
                      .current_scope = ast_ctx->global_scope,
                      .str_lit_count = 0,
                      .loop_cond_bb = NULL,
                      .loop_exit_bb = NULL,
                      .string_literals = NULL,
                      .error_count = 0,
                      .warning_count = 0};

  init_value_map(&ctx);

  // 第一遍预扫描：遍历整个 AST，找到所有字符串字面量并为它们创建全局变量。
  simple_ast_traverse(ast_ctx->root, prescan_string_literals_visitor, &ctx);

  ASTNode *root = ast_ctx->root;
  if (root && root->node_type == AST_COMPOUND_STMT) {
    // 第二遍：生成全局变量的定义。
    generate_globals(&ctx, root);

    // 第三遍：遍历顶层项，为每个函数生成代码。
    for (size_t i = 0; i < root->compound_stmt.item_count; i++) {
      ASTNode *item = root->compound_stmt.items[i];
      if (item->node_type == AST_FUNC_DECL) {
        generate_function(&ctx, item);
      }
    }
  }

  if (ctx.error_count > 0) {
    LOG_ERROR(&ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
              "IR generation failed with %d errors", ctx.error_count);
    destroy_ir_module(module); // 生成失败时，清理已创建的模块
    return NULL;
  }

  LOG_INFO(&ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
           "IR generation completed successfully with %d warnings",
           ctx.warning_count);
  return module;
}

// --- 错误报告函数 ---
static void report_generation_error(IRGenContext *ctx, const char *message,
                                    SourceLocation loc) {
  ctx->error_count++;
  add_error(&ctx->ast_ctx->errors, ERROR_SEMANTIC, message, loc);
  LOG_ERROR(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
            "IR Generation Error: %s at %d:%d", message, loc.first_line,
            loc.first_column);
}

// --- 常量初始化器生成 ---

/**
 * @brief 递归地为全局变量或常量生成初始化器的 IRValue。
 * @param ctx IR生成上下文。
 * @param type 目标类型。
 * @param init_node 指向 AST 初始化表达式的节点。
 * @return 代表常量初始化器的 IRValue。
 */
static IRValue *generate_constant_initializer(IRGenContext *ctx, Type *type,
                                              ASTNode *init_node) {
  MemoryPool *pool = ctx->module->pool;
  IRValue *const_val = (IRValue *)pool_alloc_z(pool, sizeof(IRValue));
  const_val->is_constant = true;
  const_val->type = type;

  if (type->kind == TYPE_ARRAY) {
    size_t size = type->array.dimensions[0].static_size;
    const_val->aggregate.elements =
        (IRValue **)pool_alloc(pool, size * sizeof(IRValue *));
    const_val->aggregate.count = size;

    size_t init_count = (init_node && init_node->node_type == AST_ARRAY_INIT)
                            ? init_node->array_init.elem_count
                            : 0;

    // 递归地为数组的每个元素生成初始化器
    for (size_t i = 0; i < size; ++i) {
      ASTNode *elem_init =
          (i < init_count) ? init_node->array_init.elements[i] : NULL;
      const_val->aggregate.elements[i] = generate_constant_initializer(
          ctx, type->array.element_type, elem_init);
    }
  } else {
    if (init_node && init_node->is_constant) {
      if (type->basic == BASIC_INT) {
        const_val->int_val = init_node->constant.value.int_val;
      } else {
        const_val->float_val = init_node->constant.value.float_val;
      }
    } else {
      // 默认初始化为零。
      if (type->basic == BASIC_INT) {
        const_val->int_val = 0;
      } else {
        const_val->float_val = 0.0f;
      }
    }
  }
  return const_val;
}

// --- 全局变量生成 ---
static void generate_globals(IRGenContext *ctx, ASTNode *root) {
  for (size_t i = 0; i < root->compound_stmt.item_count; ++i) {
    ASTNode *item = root->compound_stmt.items[i];
    if (item->node_type == AST_VAR_DECL || item->node_type == AST_CONST_DECL) {
      Symbol *sym = item->sym;
      if (!sym) {
        report_generation_error(ctx, "Global variable has no symbol",
                                item->loc);
        continue;
      }

      // 记录全局变量生成
      LOG_DEBUG(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
                "Generating global variable: %s", sym->name);

      // 创建全局变量的 IR 表示
      IRGlobalVariable *global = (IRGlobalVariable *)pool_alloc(
          ctx->module->pool, sizeof(IRGlobalVariable));
      global->name = sym->name;
      global->type = sym->type;
      global->is_const = (item->node_type == AST_CONST_DECL);

      ASTNode *init_node = (item->node_type == AST_VAR_DECL)
                               ? item->var_decl.init_value
                               : item->const_decl.value;
      global->initializer =
          generate_constant_initializer(ctx, sym->type, init_node);

      // 将全局变量添加到模块的链表中
      global->next = ctx->module->globals;
      ctx->module->globals = global;

      // 为全局变量创建一个代表其地址的 IRValue。
      IRValue *global_addr =
          (IRValue *)pool_alloc_z(ctx->module->pool, sizeof(IRValue));
      global_addr->type = get_ir_pointer_type(ctx, sym->type);
      global_addr->is_global = true;
      char *name_buf =
          (char *)pool_alloc(ctx->module->pool, strlen(sym->name) + 2);
      sprintf(name_buf, "%s", sym->name);
      global_addr->name = name_buf;
      map_addr(ctx, sym, global_addr); // 将符号映射到其地址
    }
  }
}

// --- 字符串字面量处理 ---

/**
 * @brief 一个AST访问者，用于预扫描并创建所有字符串字面量的全局定义。
 * @details 采用缓存机制，确保相同的字符串字面量只在IR中生成一次。
 */
static void prescan_string_literals_visitor(ASTNode *node, void *user_data) {
  if (node->node_type != AST_STRING_LITERAL)
    return;

  IRGenContext *ctx = (IRGenContext *)user_data;

  // 检查此字符串是否已经处理过
  for (StringLiteralEntry *s = ctx->string_literals; s; s = s->next) {
    if (strcmp(s->ast_value, node->string_literal.value) == 0) {
      node->sym = (Symbol *)s; // 使用 sym 字段来存储指向缓存条目的指针
      return;
    }
  }

  MemoryPool *pool = ctx->module->pool;

  // 创建一个新的全局字符串常量
  IRGlobalVariable *global_str =
      (IRGlobalVariable *)pool_alloc_z(pool, sizeof(IRGlobalVariable));

  char *global_name_buf = (char *)pool_alloc(pool, 20);
  sprintf(global_name_buf, ".str.%d", ctx->str_lit_count++);
  global_str->name = global_name_buf;
  global_str->is_const = true;

  size_t len_with_null = node->string_literal.length + 1;
  ArrayDimension dim = {.is_dynamic = false, .static_size = len_with_null};
  global_str->type = create_array_type(create_basic_type(BASIC_I8, true, pool),
                                       &dim, 1, true, pool);

  global_str->initializer = (IRValue *)pool_alloc_z(pool, sizeof(IRValue));
  global_str->initializer->is_constant = true;
  global_str->initializer->type = global_str->type;
  global_str->initializer->name =
      node->string_literal.value; // 在打印时会处理为字符串内容
  global_str->next = ctx->module->globals;
  ctx->module->globals = global_str;

  // 创建代表其地址的 IRValue
  IRValue *global_addr = (IRValue *)pool_alloc_z(pool, sizeof(IRValue));
  global_addr->type = get_ir_pointer_type(ctx, global_str->type);
  global_addr->is_global = true;
  global_addr->name = global_str->name;

  // 添加到缓存中
  StringLiteralEntry *new_entry =
      (StringLiteralEntry *)pool_alloc(pool, sizeof(StringLiteralEntry));
  new_entry->ast_value = node->string_literal.value;
  new_entry->global_var = global_addr;
  new_entry->next = ctx->string_literals;
  ctx->string_literals = new_entry;
  node->sym = (Symbol *)new_entry;
}

// --- 函数生成 ---

/**
 * @brief 为单个函数声明节点生成完整的IR代码。
 * @param ctx IR生成上下文。
 * @param func_decl_node 指向 `AST_FUNC_DECL` 节点的指针。
 */
static void generate_function(IRGenContext *ctx, ASTNode *func_decl_node) {
  FuncDeclNode *ast_func = &func_decl_node->func_decl;
  ctx->current_scope = ast_func->scope;

  // 创建 IRFunction 对象
  IRFunction *func =
      (IRFunction *)pool_alloc_z(ctx->module->pool, sizeof(IRFunction));
  func->name = pool_strdup(ctx->module->pool, ast_func->func_name);
  func->return_type = ast_func->return_type;
  func->module = ctx->module;
  func->next = ctx->module->functions;
  ctx->module->functions = func;

  // 新增：参数值列表
  func->num_args = ast_func->param_count;
  if (func->num_args > 0) {
    func->args =
        pool_alloc(ctx->module->pool, func->num_args * sizeof(IRValue *));
  }

  // 初始化 IRBuilder 并创建入口块
  ir_builder_init(&ctx->builder, func);
  IRBasicBlock *entry_bb = ir_builder_create_block(&ctx->builder, "entry");
  func->entry = entry_bb;
  ir_builder_set_insertion_block(&ctx->builder, entry_bb);

  // 生成参数 IRValue、alloca、store
  for (size_t i = 0; i < ast_func->param_count; ++i) {
    Symbol *param_sym = ast_func->params[i]->sym;
    if (!param_sym) {
      report_generation_error(ctx, "Function parameter has no symbol",
                              ast_func->params[i]->loc);
      continue;
    }
    // 1. 创建参数值
    IRValue *param_val =
        (IRValue *)pool_alloc_z(ctx->module->pool, sizeof(IRValue));
    param_val->name = param_sym->name;
    param_val->type = param_sym->type;
    param_val->is_global = false;
    func->args[i] = param_val;
    // 2. alloca
    IRInstruction *alloca_instr = ir_builder_create_alloca(
        &ctx->builder, param_sym->type, param_sym->name);
    param_val->alloca_instr = alloca_instr;
    map_addr(ctx, param_sym, alloca_instr->dest);
    // 3. store
    ir_builder_create_store(&ctx->builder, param_val, alloca_instr->dest);
  }

  // 预扫描函数体，为所有局部变量创建 `alloca` 指令
  simple_ast_traverse(ast_func->body, find_and_alloc_locals_visitor, ctx);

  // 生成函数体的所有语句
  generate_statement(ctx, ast_func->body);

  // 确保函数末尾有正确的返回指令
  if (ctx->builder.current_bb &&
      (ctx->builder.current_bb->tail == NULL ||
       (ctx->builder.current_bb->tail->opcode != IR_OP_RET &&
        ctx->builder.current_bb->tail->opcode != IR_OP_BR))) {

    if (func->return_type->kind == TYPE_VOID) {
      ir_builder_create_ret_void(&ctx->builder);
    } else {
      // 对于非 void 函数，如果控制流能到达末尾，默认返回 0 或 0.0
      IRValue *zero_ret =
          (func->return_type->basic == BASIC_INT)
              ? ir_builder_create_const_int(&ctx->builder, 0)
              : ir_builder_create_const_float(&ctx->builder, 0.0f);
      ir_builder_create_ret(&ctx->builder, zero_ret);
    }
  }
}

/**
 * @brief 一个AST访问者，用于在生成函数体代码之前，先为所有局部变量创建 `alloca`
 * 指令。
 * @details 这确保了所有局部变量的 `alloca` 指令都集中在函数入口块的顶部，
 *          这是 `mem2reg` 优化遍的必要前置条件。
 */
static void find_and_alloc_locals_visitor(ASTNode *node, void *user_data) {
  if (node->node_type != AST_VAR_DECL && node->node_type != AST_CONST_DECL)
    return;
  IRGenContext *ctx = (IRGenContext *)user_data;
  Symbol *sym = node->sym;
  assert(sym != NULL);
  // 如果已经为其分配了地址（例如，作为函数参数），则跳过
  if (find_addr(ctx, sym))
    return;
  IRInstruction *alloca_instr =
      ir_builder_create_alloca(&ctx->builder, sym->type, sym->name);
  map_addr(ctx, sym, alloca_instr->dest);
}

/**
 * @brief 递归地为单个 AST 语句节点生成 IR 代码。
 * @param ctx IR生成上下文。
 * @param stmt_node 指向语句节点的指针。
 */
static void generate_statement(IRGenContext *ctx, ASTNode *stmt_node) {
  if (!stmt_node)
    return;
  SymbolTable *prev_scope = ctx->current_scope;
  if (stmt_node->node_type == AST_COMPOUND_STMT) {
    ctx->current_scope = stmt_node->compound_stmt.scope;
  }

  switch (stmt_node->node_type) {
  case AST_VAR_DECL:
    // 如果变量有初始化，则生成赋值代码
    if (stmt_node->var_decl.init_value) {
      Symbol *sym = stmt_node->sym;
      IRValue *addr = find_addr(ctx, sym);
      assert(addr);
      if (sym->type->kind == TYPE_ARRAY) {
        generate_local_array_init(ctx, addr, sym->type,
                                  stmt_node->var_decl.init_value);
      } else {
        IRValue *init_val =
            generate_expression(ctx, stmt_node->var_decl.init_value, false);
        init_val = create_type_conversion(
            ctx, init_val, stmt_node->var_decl.init_value->eval_type,
            sym->type);
        ir_builder_create_store(&ctx->builder, init_val, addr);
      }
    }
    break;
  case AST_ASSIGN_STMT: {
    IRValue *addr = generate_expression(ctx, stmt_node->assign_stmt.lval, true);
    IRValue *rval =
        generate_expression(ctx, stmt_node->assign_stmt.expr, false);
    rval = create_type_conversion(ctx, rval,
                                  stmt_node->assign_stmt.expr->eval_type,
                                  stmt_node->assign_stmt.lval->eval_type);
    ir_builder_create_store(&ctx->builder, rval, addr);
    break;
  }
  case AST_EXPR_STMT:
    if (stmt_node->expr_stmt.expr)
      generate_expression(ctx, stmt_node->expr_stmt.expr, false);
    break;
  case AST_COMPOUND_STMT:
    for (size_t i = 0; i < stmt_node->compound_stmt.item_count; ++i)
      generate_statement(ctx, stmt_node->compound_stmt.items[i]);
    break;
  case AST_IF_STMT: {
    IRValue *cond = generate_expression(ctx, stmt_node->if_stmt.cond, false);
    // 将条件值与0比较，得到 i1 类型的布尔值
    IRValue *cmp = ir_builder_create_icmp(
                       &ctx->builder, "ne", cond,
                       ir_builder_create_const_int(&ctx->builder, 0), "ifcond")
                       ->dest;

    IRBasicBlock *then_bb = ir_builder_create_block(&ctx->builder, "if.then");
    IRBasicBlock *else_bb =
        stmt_node->if_stmt.else_stmt
            ? ir_builder_create_block(&ctx->builder, "if.else")
            : NULL;
    IRBasicBlock *end_bb = ir_builder_create_block(&ctx->builder, "if.end");

    // 创建条件分支
    ir_builder_create_cond_br(&ctx->builder, cmp, then_bb,
                              else_bb ? else_bb : end_bb);

    // 生成 'then' 分支的代码
    ir_builder_set_insertion_block(&ctx->builder, then_bb);
    generate_statement(ctx, stmt_node->if_stmt.then_stmt);
    if (ctx->builder.current_bb->tail == NULL)
      ir_builder_create_br(&ctx->builder, end_bb);

    // 如果有 'else' 分支，生成其代码
    if (else_bb) {
      ir_builder_set_insertion_block(&ctx->builder, else_bb);
      generate_statement(ctx, stmt_node->if_stmt.else_stmt);
      if (ctx->builder.current_bb->tail == NULL)
        ir_builder_create_br(&ctx->builder, end_bb);
    }
    // 将插入点移动到 'end' 块，继续生成后续代码
    ir_builder_set_insertion_block(&ctx->builder, end_bb);
    break;
  }
  case AST_WHILE_STMT: {
    IRBasicBlock *cond_bb =
        ir_builder_create_block(&ctx->builder, "while.cond");
    IRBasicBlock *body_bb =
        ir_builder_create_block(&ctx->builder, "while.body");
    IRBasicBlock *end_bb = ir_builder_create_block(&ctx->builder, "while.end");
    // 保存并更新当前的循环上下文（用于处理嵌套循环中的 break/continue）
    IRBasicBlock *prev_cond = ctx->loop_cond_bb, *prev_exit = ctx->loop_exit_bb;
    ctx->loop_cond_bb = cond_bb;
    ctx->loop_exit_bb = end_bb;

    // 跳转到条件检查块
    ir_builder_create_br(&ctx->builder, cond_bb);
    // 生成条件检查块的代码
    ir_builder_set_insertion_block(&ctx->builder, cond_bb);
    IRValue *cond = generate_expression(ctx, stmt_node->while_stmt.cond, false);
    IRValue *cmp =
        ir_builder_create_icmp(&ctx->builder, "ne", cond,
                               ir_builder_create_const_int(&ctx->builder, 0),
                               "loopcond")
            ->dest;
    ir_builder_create_cond_br(&ctx->builder, cmp, body_bb, end_bb);

    // 生成循环体代码
    ir_builder_set_insertion_block(&ctx->builder, body_bb);
    generate_statement(ctx, stmt_node->while_stmt.body);
    if (ctx->builder.current_bb->tail == NULL)
      ir_builder_create_br(&ctx->builder, cond_bb); // 循环体末尾跳回条件检查

    // 将插入点移动到循环结束块
    ir_builder_set_insertion_block(&ctx->builder, end_bb);
    // 恢复外层循环的上下文
    ctx->loop_cond_bb = prev_cond;
    ctx->loop_exit_bb = prev_exit;
    break;
  }
  case AST_BREAK_STMT:
    assert(ctx->loop_exit_bb);
    ir_builder_create_br(&ctx->builder, ctx->loop_exit_bb);
    // break 之后是不可达代码，创建一个新块
    ir_builder_set_insertion_block(
        &ctx->builder,
        ir_builder_create_block(&ctx->builder, "unreachable.br"));
    break;
  case AST_CONTINUE_STMT:
    assert(ctx->loop_cond_bb);
    ir_builder_create_br(&ctx->builder, ctx->loop_cond_bb);
    // continue 之后是不可达代码，创建一个新块
    ir_builder_set_insertion_block(
        &ctx->builder,
        ir_builder_create_block(&ctx->builder, "unreachable.cont"));
    break;
  case AST_RETURN_STMT:
    if (stmt_node->return_stmt.value) {
      IRValue *ret_val =
          generate_expression(ctx, stmt_node->return_stmt.value, false);
      ret_val = create_type_conversion(ctx, ret_val,
                                       stmt_node->return_stmt.value->eval_type,
                                       ctx->builder.current_func->return_type);
      ir_builder_create_ret(&ctx->builder, ret_val);
    } else {
      ir_builder_create_ret_void(&ctx->builder);
    }
    // return 之后是不可达代码，创建一个新块
    ir_builder_set_insertion_block(
        &ctx->builder,
        ir_builder_create_block(&ctx->builder, "unreachable.ret"));
    break;
  default:
    LOG_WARN(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
             "IR generation for statement type %d not implemented.",
             stmt_node->node_type);
    break;
  }
  if (prev_scope)
    ctx->current_scope = prev_scope;
}

/**
 * @brief 递归地为单个 AST 表达式节点生成 IR 代码。
 * @param ctx IR生成上下文。
 * @param expr_node 指向表达式节点的指针。
 * @param want_address 如果为
 * true，返回表达式的地址（左值）；否则返回其计算结果（右值）。
 * @return 代表地址或值的 `IRValue*`。
 */
static IRValue *generate_expression(IRGenContext *ctx, ASTNode *expr_node,
                                    bool want_address) {
  if (!expr_node)
    return NULL;
  IRBuilder *builder = &ctx->builder;

  // 特殊处理短路求值
  if (expr_node->node_type == AST_BINARY_EXPR &&
      (operator_type_to_ir_opcode(expr_node->binary_expr.op, false) ==
           IR_OP_AND ||
       operator_type_to_ir_opcode(expr_node->binary_expr.op, false) ==
           IR_OP_OR)) {
    assert(!want_address);
    IRBasicBlock *start_bb = builder->current_bb;
    IRBasicBlock *rhs_bb = ir_builder_create_block(builder, "sc.rhs");
    IRBasicBlock *end_bb = ir_builder_create_block(builder, "sc.end");

    IRValue *lhs_val =
        generate_expression(ctx, expr_node->binary_expr.left, false);
    IRValue *lhs_cmp = ir_builder_create_icmp(
                           builder, "ne", lhs_val,
                           ir_builder_create_const_int(builder, 0), "lhs.cmp")
                           ->dest;

    if (operator_type_to_ir_opcode(expr_node->binary_expr.op, false) ==
        IR_OP_AND) {
      ir_builder_create_cond_br(builder, lhs_cmp, rhs_bb, end_bb);
    } else {
      ir_builder_create_cond_br(builder, lhs_cmp, end_bb, rhs_bb);
    }

    ir_builder_set_insertion_block(builder, rhs_bb);
    IRValue *rhs_val =
        generate_expression(ctx, expr_node->binary_expr.right, false);
    IRValue *rhs_cmp = ir_builder_create_icmp(
                           builder, "ne", rhs_val,
                           ir_builder_create_const_int(builder, 0), "rhs.cmp")
                           ->dest;
    IRBasicBlock *rhs_bb_final = builder->current_bb;
    if (builder->current_bb->tail == NULL)
      ir_builder_create_br(builder, end_bb);

    ir_builder_set_insertion_block(builder, end_bb);
    Type *i1_type = create_basic_type(BASIC_I1, false, builder->module->pool);
    IRInstruction *phi = ir_builder_create_phi(builder, i1_type, "sc.phi");
    if (operator_type_to_ir_opcode(expr_node->binary_expr.op, false) ==
        IR_OP_AND) {
      ir_phi_add_incoming(phi, create_constant_i1(false, builder->module->pool),
                          start_bb);
      ir_phi_add_incoming(phi, rhs_cmp, rhs_bb_final);
    } else {
      ir_phi_add_incoming(phi, create_constant_i1(true, builder->module->pool),
                          start_bb);
      ir_phi_add_incoming(phi, rhs_cmp, rhs_bb_final);
    }
    return ir_builder_create_zext(
               builder, phi->dest,
               create_basic_type(BASIC_INT, false, builder->module->pool),
               "sc.res")
        ->dest;
  }

  switch (expr_node->node_type) {
  case AST_CONSTANT:
    return (expr_node->constant.type == CONST_INT)
               ? ir_builder_create_const_int(builder,
                                             expr_node->constant.value.int_val)
               : ir_builder_create_const_float(
                     builder, expr_node->constant.value.float_val);
  case AST_IDENTIFIER: {
    Symbol *sym = expr_node->sym;
    IRValue *addr = find_addr(ctx, sym);
    if (want_address)
      return addr;
    if (sym->type->kind == TYPE_ARRAY) {
      report_generation_error(
          ctx, "Internal: Attempt to evaluate an array identifier as a value",
          expr_node->loc);
      return NULL;
    }

    // SysY-2022常量折叠：对于const变量，直接返回常量值而不是生成LOAD指令
    if (sym->is_const && sym->is_evaluated) {
      LOG_TRACE(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
                "Folding const variable %s to constant value", sym->name);

      if (sym->type->kind == TYPE_BASIC) {
        if (sym->type->basic == BASIC_INT) {
          return ir_builder_create_const_int(builder, sym->const_val.int_val);
        } else if (sym->type->basic == BASIC_FLOAT) {
          return ir_builder_create_const_float(builder,
                                               sym->const_val.float_val);
        }
      }
    }

    return ir_builder_create_load(builder, addr, sym->name)->dest;
  }
  case AST_BINARY_EXPR: {
    IRValue *left =
        generate_expression(ctx, expr_node->binary_expr.left, false);
    IRValue *right =
        generate_expression(ctx, expr_node->binary_expr.right, false);
    bool is_float = (expr_node->eval_type->basic == BASIC_FLOAT);
    Type *common_type = expr_node->eval_type;
    // 自动处理类型转换
    left = create_type_conversion(
        ctx, left, expr_node->binary_expr.left->eval_type, common_type);
    right = create_type_conversion(
        ctx, right, expr_node->binary_expr.right->eval_type, common_type);

    switch (operator_type_to_ir_opcode(expr_node->binary_expr.op, is_float)) {
    case IR_OP_ADD:
      return is_float
                 ? ir_builder_create_fadd(builder, left, right, "faddtmp")->dest
                 : ir_builder_create_add(builder, left, right, "addtmp")->dest;
    case IR_OP_SUB:
      return is_float
                 ? ir_builder_create_fsub(builder, left, right, "fsubtmp")->dest
                 : ir_builder_create_sub(builder, left, right, "subtmp")->dest;
    case IR_OP_MUL:
      return is_float
                 ? ir_builder_create_fmul(builder, left, right, "fmultmp")->dest
                 : ir_builder_create_mul(builder, left, right, "multmp")->dest;
    case IR_OP_SDIV:
      return is_float
                 ? ir_builder_create_fdiv(builder, left, right, "fdivtmp")->dest
                 : ir_builder_create_sdiv(builder, left, right, "sdivtmp")
                       ->dest;
    case IR_OP_SREM:
      return ir_builder_create_srem(builder, left, right, "sremtmp")->dest;
    case IR_OP_ICMP: {
      const char *cond = get_icmp_cond(expr_node->binary_expr.op);
      IRInstruction *cmp_instr =
          ir_builder_create_icmp(builder, cond, left, right, "icmptmp");
      return ir_builder_create_zext(
                 builder, cmp_instr->dest,
                 create_basic_type(BASIC_INT, false, builder->module->pool),
                 "booltmp")
          ->dest;
    }
    case IR_OP_FCMP: {
      const char *cond = get_fcmp_cond(expr_node->binary_expr.op);
      IRInstruction *cmp_instr =
          ir_builder_create_fcmp(builder, cond, left, right, "fcmptmp");
      return ir_builder_create_zext(
                 builder, cmp_instr->dest,
                 create_basic_type(BASIC_INT, false, builder->module->pool),
                 "booltmp")
          ->dest;
    }
    default:
      assert(0 && "Unknown binary operator");
      return NULL;
    }
  }
  case AST_UNARY_EXPR: {
    IRValue *operand =
        generate_expression(ctx, expr_node->unary_expr.operand, false);
    bool is_float = (expr_node->eval_type->basic == BASIC_FLOAT);
    switch (expr_node->unary_expr.op) {
    case OP_NEG:
      // 通过 0 - operand 实现负号
      return is_float
                 ? ir_builder_create_fsub(
                       builder, ir_builder_create_const_float(builder, 0.0f),
                       operand, "fnegtmp")
                       ->dest
                 : ir_builder_create_sub(
                       builder, ir_builder_create_const_int(builder, 0),
                       operand, "negtmp")
                       ->dest;
    case OP_POS:
      return operand; // 正号是无操作
    case OP_NOT: {
      // 通过 icmp eq operand, 0 实现逻辑非
      IRInstruction *cmp_instr = ir_builder_create_icmp(
          builder, "eq", operand, ir_builder_create_const_int(builder, 0),
          "nottmp");
      return ir_builder_create_zext(
                 builder, cmp_instr->dest,
                 create_basic_type(BASIC_INT, false, builder->module->pool),
                 "booltmp")
          ->dest;
    }
    default:
      assert(0 && "Unknown unary operator");
      return NULL;
    }
  }
  case AST_CALL_EXPR: {
    Symbol *func_sym = expr_node->sym;
    IRValue *args[expr_node->call_expr.arg_count];

    // 记录函数调用
    LOG_DEBUG(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
              "Generating function call: %s with %zu arguments", func_sym->name,
              expr_node->call_expr.arg_count);

    for (size_t i = 0; i < expr_node->call_expr.arg_count; ++i) {
      ASTNode *arg_node = expr_node->call_expr.args[i];
      args[i] = generate_expression(ctx, arg_node, false);

      // 改进的参数类型转换处理
      if (func_sym->type->function.is_variadic) {
        // 可变参数函数的特殊处理（符合C调用约定）
        if (i >= func_sym->type->function.param_count) {
          // 可变参数部分：按照C调用约定进行类型提升
          if (args[i]->type->kind == TYPE_BASIC) {
            if (args[i]->type->basic == BASIC_FLOAT) {
              // float -> double (可变参数中的float提升为double)
              Type *double_type =
                  create_basic_type(BASIC_DOUBLE, false, builder->module->pool);
              args[i] = ir_builder_create_fpext(builder, args[i], double_type,
                                                "vararg.fpext")
                            ->dest;
              LOG_TRACE(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
                        "Promoting float to double for variadic argument %zu",
                        i);
            } else if (args[i]->type->basic == BASIC_I8 ||
                       args[i]->type->basic == BASIC_I1) {
              // 小整数类型 -> int (整数提升)
              Type *int_type =
                  create_basic_type(BASIC_INT, false, builder->module->pool);
              args[i] = ir_builder_create_sext(builder, args[i], int_type,
                                               "vararg.sext")
                            ->dest;
              LOG_TRACE(
                  &ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
                  "Promoting small integer to int for variadic argument %zu",
                  i);
            }
          }
        } else {
          // 固定参数部分：正常类型转换
          args[i] =
              create_type_conversion(ctx, args[i], arg_node->eval_type,
                                     func_sym->type->function.param_types[i]);
        }
      } else {
        // 非可变参数函数：严格类型转换
        if (i < func_sym->type->function.param_count) {
          args[i] =
              create_type_conversion(ctx, args[i], arg_node->eval_type,
                                     func_sym->type->function.param_types[i]);
        } else {
          LOG_WARN(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
                   "Too many arguments for non-variadic function %s",
                   func_sym->name);
        }
      }
    }

    IRInstruction *call_instr =
        ir_builder_create_call(builder, find_addr(ctx, func_sym), args,
                               expr_node->call_expr.arg_count, "calltmp");
    return call_instr->dest;
  }
  case AST_ARRAY_ACCESS: {
    IRValue *base_addr =
        generate_expression(ctx, expr_node->array_access.array, true);
    IRValue *index_val =
        generate_expression(ctx, expr_node->array_access.index, false);

    // 添加数组边界检查（SysY-2022要求）
    Type *array_type = expr_node->array_access.array->eval_type;
    if (array_type && array_type->kind == TYPE_ARRAY &&
        !array_type->array.dimensions[0].is_dynamic) {

      int array_size = array_type->array.dimensions[0].static_size;
      IRValue *size_const = ir_builder_create_const_int(builder, array_size);

      // 检查索引是否为负数
      IRValue *zero_const = ir_builder_create_const_int(builder, 0);
      IRValue *neg_check = ir_builder_create_icmp(builder, "slt", index_val,
                                                  zero_const, "neg_check")
                               ->dest;

      // 检查索引是否超出上界
      IRValue *bound_check = ir_builder_create_icmp(builder, "sge", index_val,
                                                    size_const, "bound_check")
                                 ->dest;

      // 合并两个检查条件
      IRValue *out_of_bounds =
          ir_builder_create_or(builder, neg_check, bound_check, "out_of_bounds")
              ->dest;

      // 创建边界检查的基本块
      IRBasicBlock *bounds_ok_bb =
          ir_builder_create_block(builder, "bounds_ok");
      IRBasicBlock *bounds_fail_bb =
          ir_builder_create_block(builder, "bounds_fail");
      IRBasicBlock *continue_bb =
          ir_builder_create_block(builder, "array_continue");

      // 条件分支
      ir_builder_create_cond_br(builder, out_of_bounds, bounds_fail_bb,
                                bounds_ok_bb);

      // 边界检查失败块 - 调用运行时错误处理
      ir_builder_set_insertion_block(builder, bounds_fail_bb);

      // 1. 查找 putf 函数的 IRValue
      Symbol *putf_sym = find_symbol(ctx->ast_ctx->global_scope, "putf");
      IRValue *putf_func = find_addr(ctx, putf_sym);
      assert(putf_func && "putf function not found in IR generation");

      // 2. 创建错误信息字符串常量
      IRValue *error_msg =
          (IRValue *)pool_alloc(builder->module->pool, sizeof(IRValue));
      error_msg->is_constant = true;
      error_msg->type = create_basic_type(
          BASIC_INT, false,
          builder->module->pool); // 使用 int 类型作为字符串指针
      error_msg->name = pool_strdup(builder->module->pool,
                                    "Array index out of bounds at line %d\n");

      // 3. 创建行号常量
      IRValue *line_num =
          ir_builder_create_const_int(builder, expr_node->loc.first_line);

      // 4. 调用 putf
      IRValue *args[] = {error_msg, line_num};
      ir_builder_create_call(builder, putf_func, args, 2, NULL);

      // 5. 在错误处理后，应该有一个终结符，比如调用 exit() 或进入一个无限循环。
      // 为了简单，可以先跳转到 continue_bb。
      ir_builder_create_br(builder, continue_bb);

      // 边界检查成功块
      ir_builder_set_insertion_block(builder, bounds_ok_bb);
      ir_builder_create_br(builder, continue_bb);

      // 继续正常的数组访问
      ir_builder_set_insertion_block(builder, continue_bb);
    }

    IRValue *indices[] = {index_val};
    IRValue *elem_ptr =
        ir_builder_create_gep(builder, base_addr, indices, 1, "elemptr")->dest;
    if (want_address || expr_node->eval_type->kind == TYPE_ARRAY)
      return elem_ptr;
    return ir_builder_create_load(builder, elem_ptr, "elem")->dest;
  }
  case AST_STRING_LITERAL: {
    assert(!want_address);
    StringLiteralEntry *entry = (StringLiteralEntry *)expr_node->sym;
    IRValue *indices[] = {create_constant_i64(0, builder->module->pool),
                          create_constant_i64(0, builder->module->pool)};
    return ir_builder_create_gep(builder, entry->global_var, indices, 2,
                                 "strptr")
        ->dest;
  }
  default:
    LOG_WARN(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
             "IR generation for expression type %d not implemented.",
             expr_node->node_type);
    return NULL;
  }
}

/**
 * @brief 为本地数组的初始化列表生成 IR 代码。
 * @details
 * 这是一个复杂的函数，负责处理在函数内部声明的数组的初始化。
 * 它能处理嵌套的初始化列表，并为未显式初始化的元素生成零值填充。
 *
 * @param ctx IR 生成上下文。
 * @param array_addr 指向数组在栈上分配的内存的起始地址的 IRValue。
 * @param array_type 数组的 AST 类型信息。
 * @param init_list 指向 `AST_ARRAY_INIT` 节点的初始化列表。
 */
static void generate_local_array_init(IRGenContext *ctx, IRValue *array_addr,
                                      Type *array_type, ASTNode *init_list) {
  if (!init_list || init_list->node_type != AST_ARRAY_INIT) {
    return; // 如果没有初始化列表，则不进行任何操作（保持未初始化状态）
  }

  IRBuilder *builder = &ctx->builder;
  Type *elem_type = array_type->array.element_type;
  size_t declared_size = array_type->array.dimensions[0].static_size;
  size_t init_count = init_list->array_init.elem_count;

  // 1. 处理显式提供的初始化项
  for (size_t i = 0; i < init_count; ++i) {
    if (i >= declared_size) {
      const char *var_name = "unknown_array";
      Symbol *sym = find_symbol_for_addr(ctx, array_addr);
      if (sym) {
        var_name = sym->name;
      }
      LOG_WARN(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
               "Initializer list for array '%s' exceeds its declared size, "
               "ignoring extra elements.",
               var_name);
      break; // 初始化项超出了数组大小，停止处理
    }

    ASTNode *item_init = init_list->array_init.elements[i];

    // 计算当前元素的地址：GEP(array_addr, 0, i)
    IRValue *indices[] = {create_constant_i64(0, builder->module->pool),
                          create_constant_i64(i, builder->module->pool)};
    IRValue *elem_ptr =
        ir_builder_create_gep(builder, array_addr, indices, 2, "init.gep")
            ->dest;

    if (elem_type->kind == TYPE_ARRAY) {
      // 如果元素本身也是一个数组（即多维数组），则递归处理其初始化
      generate_local_array_init(ctx, elem_ptr, elem_type, item_init);
    } else {
      // 如果元素是标量，则生成表达式并存储
      IRValue *rval = generate_expression(ctx, item_init, false);
      rval = create_type_conversion(ctx, rval, item_init->eval_type, elem_type);
      ir_builder_create_store(builder, rval, elem_ptr);
    }
  }

  // 2. 处理未显式初始化的部分，用零填充
  if (init_count < declared_size) {
    LOG_DEBUG(
        &ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
        "Generating zero-initializer stores for array from index %zu to %zu.",
        init_count, declared_size - 1);
    IRValue *zero_val = (elem_type->basic == BASIC_INT)
                            ? ir_builder_create_const_int(builder, 0)
                            : ir_builder_create_const_float(builder, 0.0f);
    for (size_t i = init_count; i < declared_size; ++i) {
      IRValue *indices[] = {create_constant_i64(0, builder->module->pool),
                            create_constant_i64(i, builder->module->pool)};
      IRValue *elem_ptr =
          ir_builder_create_gep(builder, array_addr, indices, 2, "zero.gep")
              ->dest;
      ir_builder_create_store(builder, zero_val, elem_ptr);
    }
  }
}

// --- 最终的辅助函数 ---

/**
 * @brief 初始化值映射表。
 * @param ctx IR生成上下文。
 */
static void init_value_map(IRGenContext *ctx) {
  value_map_init(&ctx->value_map, ctx->module->pool);
}

/**
 * @brief 在值映射表中将一个 AST 符号映射到其在 IR 中的地址。
 * @param ctx IR生成上下文。
 * @param sym AST 符号。
 * @param addr IR 中代表该符号地址的 IRValue。
 */
static void map_addr(IRGenContext *ctx, Symbol *sym, IRValue *addr) {
  // 记录符号地址映射操作
  LOG_TRACE(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
            "Mapping symbol %s to address", sym->name ? sym->name : "unnamed");

  // 检查ValueMap是否需要扩容（在扩容前记录日志）
  if (ctx->value_map.count >= ctx->value_map.capacity) {
    LOG_DEBUG(&ctx->ast_ctx->log_config, LOG_CATEGORY_MEMORY,
              "ValueMap expanding from capacity %d to handle symbol mapping",
              ctx->value_map.capacity);
  }

  // 使用 Symbol 指针本身作为 Key，因为它是唯一的
  value_map_put(&ctx->value_map, (IRValue *)sym, addr,
                &ctx->ast_ctx->log_config);
}

/**
 * @brief 在值映射表中查找一个 AST 符号对应的 IR 地址。
 * @param ctx IR生成上下文。
 * @param sym 要查找的 AST 符号。
 * @return 找到则返回对应的 `IRValue*`，否则返回 `NULL`。
 */
static IRValue *find_addr(IRGenContext *ctx, Symbol *sym) {
  return value_map_get(&ctx->value_map, (IRValue *)sym,
                       &ctx->ast_ctx->log_config);
}

/**
 * @brief 根据 IR 地址反向查找对应的 AST 符号。
 * @details 主要用于调试和生成更具可读性的错误信息。
 * @param ctx IR生成上下文。
 * @param addr 要查找的地址 `IRValue*`。
 * @return 找到则返回对应的 `Symbol*`，否则返回 `NULL`。
 */
static Symbol *find_symbol_for_addr(IRGenContext *ctx, IRValue *addr) {
  for (int i = 0; i < ctx->value_map.count; ++i) {
    if (ctx->value_map.entries[i].new_val == addr) {
      return (Symbol *)ctx->value_map.entries[i].old_val;
    }
  }
  return NULL;
}

/**
 * @brief 根据一个基本类型，创建一个指向该类型的指针类型。
 * @param ctx IR生成上下文。
 * @param base_type 基础类型。
 * @return 指向 `base_type` 的新指针类型。
 */
static Type *get_ir_pointer_type(IRGenContext *ctx, Type *base_type) {
  return create_pointer_type(base_type, false, ctx->module->pool);
}

/**
 * @brief 在需要时，生成类型转换指令（int <-> float, i8 -> i32）。
 * @param ctx IR生成上下文。
 * @param src_val 源值的 IRValue。
 * @param src_type 源值的 AST 类型。
 * @param dest_type 目标值的 AST 类型。
 * @return 转换后的 IRValue。如果不需要转换，则返回原始的 `src_val`。
 */
static IRValue *create_type_conversion(IRGenContext *ctx, IRValue *src_val,
                                       Type *src_type, Type *dest_type) {
  if (!src_type || !dest_type || is_type_same(src_type, dest_type, true))
    return src_val;

  // 记录类型转换尝试
  LOG_TRACE(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
            "Attempting type conversion from %s to %s",
            src_type->kind == TYPE_BASIC ? "basic_type" : "complex_type",
            dest_type->kind == TYPE_BASIC ? "basic_type" : "complex_type");

  if (src_type->kind != TYPE_BASIC || dest_type->kind != TYPE_BASIC) {
    if (!is_type_same(src_type, dest_type, true)) {
      LOG_WARN(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
               "Unsupported type conversion attempted.");
      return NULL;
    }
    return src_val;
  }
  IRBuilder *builder = &ctx->builder;

  // 处理整数扩展/截断
  if (src_type->basic == BASIC_I8 && dest_type->basic == BASIC_INT) {
    // i8 -> i32 (零扩展，因为SysY/C中的char通常是无符号的)
    return ir_builder_create_zext(builder, src_val, dest_type, "zext.tmp")
        ->dest;
  }
  if (src_type->basic == BASIC_I1 && dest_type->basic == BASIC_INT) {
    // i1 -> i32 (零扩展)
    return ir_builder_create_zext(builder, src_val, dest_type, "zext.tmp")
        ->dest;
  }
  if (src_type->basic == BASIC_INT && dest_type->basic == BASIC_I8) {
    // i32 -> i8 (截断)
    return ir_builder_create_trunc(builder, src_val, dest_type, "trunc.tmp")
        ->dest;
  }
  if (src_type->basic == BASIC_INT && dest_type->basic == BASIC_I1) {
    // i32 -> i1 (截断)
    return ir_builder_create_trunc(builder, src_val, dest_type, "trunc.tmp")
        ->dest;
  }

  // int -> float
  if (src_type->basic == BASIC_INT && dest_type->basic == BASIC_FLOAT)
    return ir_builder_create_sitofp(builder, src_val, dest_type, "i2f.conv")
        ->dest;
  // float -> int
  if (src_type->basic == BASIC_FLOAT && dest_type->basic == BASIC_INT)
    return ir_builder_create_fptosi(builder, src_val, dest_type, "f2i.conv")
        ->dest;

  // 浮点数扩展/截断
  if (src_type->basic == BASIC_FLOAT && dest_type->basic == BASIC_DOUBLE) {
    // float -> double (扩展)
    return ir_builder_create_fpext(builder, src_val, dest_type, "fpext.tmp")
        ->dest;
  }
  if (src_type->basic == BASIC_DOUBLE && dest_type->basic == BASIC_FLOAT) {
    // double -> float (截断) - 使用fptrunc指令
    LOG_TRACE(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
              "Converting double to float using FPTRUNC");
    IRInstruction *instr =
        ir_builder_create_fptrunc(builder, src_val, dest_type, "fptrunc.tmp");
    if (instr && instr->dest) {
      return instr->dest;
    }
    LOG_WARN(&ctx->ast_ctx->log_config, LOG_CATEGORY_IR_GEN,
             "Failed to create FPTRUNC instruction.");
    return NULL;
  }

  return src_val;
}

// --- 用 traverse_ast 作为 simple_ast_traverse 的实现
static void simple_ast_traverse(ASTNode *node,
                                void (*visitor)(ASTNode *, void *),
                                void *user_data) {
  // 只做前序遍历
  if (!node)
    return;
  visitor(node, user_data);
  switch (node->node_type) {
  case AST_COMPOUND_STMT:
    for (size_t i = 0; i < node->compound_stmt.item_count; i++)
      simple_ast_traverse(node->compound_stmt.items[i], visitor, user_data);
    break;
  case AST_FUNC_DECL:
    for (size_t i = 0; i < node->func_decl.param_count; ++i)
      simple_ast_traverse(node->func_decl.params[i], visitor, user_data);
    simple_ast_traverse(node->func_decl.body, visitor, user_data);
    break;
  case AST_VAR_DECL:
    simple_ast_traverse(node->var_decl.init_value, visitor, user_data);
    break;
  case AST_CONST_DECL:
    simple_ast_traverse(node->const_decl.value, visitor, user_data);
    break;
  case AST_IF_STMT:
    simple_ast_traverse(node->if_stmt.cond, visitor, user_data);
    simple_ast_traverse(node->if_stmt.then_stmt, visitor, user_data);
    if (node->if_stmt.else_stmt)
      simple_ast_traverse(node->if_stmt.else_stmt, visitor, user_data);
    break;
  case AST_WHILE_STMT:
    simple_ast_traverse(node->while_stmt.cond, visitor, user_data);
    simple_ast_traverse(node->while_stmt.body, visitor, user_data);
    break;
  case AST_RETURN_STMT:
    simple_ast_traverse(node->return_stmt.value, visitor, user_data);
    break;
  case AST_EXPR_STMT:
    simple_ast_traverse(node->expr_stmt.expr, visitor, user_data);
    break;
  case AST_ASSIGN_STMT:
    simple_ast_traverse(node->assign_stmt.lval, visitor, user_data);
    simple_ast_traverse(node->assign_stmt.expr, visitor, user_data);
    break;
  case AST_BINARY_EXPR:
    simple_ast_traverse(node->binary_expr.left, visitor, user_data);
    simple_ast_traverse(node->binary_expr.right, visitor, user_data);
    break;
  case AST_UNARY_EXPR:
    simple_ast_traverse(node->unary_expr.operand, visitor, user_data);
    break;
  case AST_CALL_EXPR:
    simple_ast_traverse(node->call_expr.callee_expr, visitor, user_data);
    for (size_t i = 0; i < node->call_expr.arg_count; ++i)
      simple_ast_traverse(node->call_expr.args[i], visitor, user_data);
    break;
  case AST_ARRAY_ACCESS:
    simple_ast_traverse(node->array_access.array, visitor, user_data);
    simple_ast_traverse(node->array_access.index, visitor, user_data);
    break;
  case AST_ARRAY_INIT:
    for (size_t i = 0; i < node->array_init.elem_count; ++i)
      simple_ast_traverse(node->array_init.elements[i], visitor, user_data);
    break;
  default:
    break;
  }
}

// 常用实现：将 OP_EQ/OP_NE/OP_LT/OP_GT/OP_LE/OP_GE 映射为
// "eq"/"ne"/"slt"/"sgt"/"sle"/"sge"
static const char *get_icmp_cond(OperatorType op) {
  switch (op) {
  case OP_EQ:
    return "eq";
  case OP_NE:
    return "ne";
  case OP_LT:
    return "slt";
  case OP_GT:
    return "sgt";
  case OP_LE:
    return "sle";
  case OP_GE:
    return "sge";
  default:
    return "unknown";
  }
}
static const char *get_fcmp_cond(OperatorType op) {
  switch (op) {
  case OP_EQ:
    return "oeq";
  case OP_NE:
    return "one";
  case OP_LT:
    return "olt";
  case OP_GT:
    return "ogt";
  case OP_LE:
    return "ole";
  case OP_GE:
    return "oge";
  default:
    return "unknown";
  }
}
static Opcode operator_type_to_ir_opcode(OperatorType op, bool is_float) {
  switch (op) {
  case OP_ADD:
    return IR_OP_ADD;
  case OP_SUB:
    return IR_OP_SUB;
  case OP_MUL:
    return IR_OP_MUL;
  case OP_DIV:
    return IR_OP_SDIV;
  case OP_MOD:
    return IR_OP_SREM;
  case OP_EQ:
  case OP_NE:
  case OP_LT:
  case OP_GT:
  case OP_LE:
  case OP_GE:
    return is_float ? IR_OP_FCMP : IR_OP_ICMP;
  case OP_AND:
    return IR_OP_AND;
  case OP_OR:
    return IR_OP_OR;
  default:
    return IR_OP_UNKNOWN;
  }
}