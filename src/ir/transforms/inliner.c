/**
 * @file inliner.c
 * @brief 实现一个函数内联（Function Inlining）优化遍。
 * @details
 * 本文件实现了函数内联的核心逻辑。函数内联是一种重要的过程间优化，它将被调用函数
 * 的函数体复制到调用点，以消除函数调用的开销并创造更多的过程内优化机会。
 *
 * 算法流程：
 * 1. **收集调用点**：遍历模块中的所有函数，找到所有符合内联条件的函数调用指令。
 *     决策基于启发式规则，如函数大小（避免代码过度膨胀）和是否递归（避免无限内联）。
 * 2.  **执行内联**：对每个选定的调用点执行以下操作：
 *     a. **克隆函数体**：将被调用函数的所有基本块和指令进行深拷贝。
 *     b.
 * **值重映射**：创建一个映射表，将被调用函数的参数映射为调用点传入的实参，
 *        并将克隆出的指令结果映射到新的SSA值。
 *     c.
 * **CFG重连接**：在调用点处切分基本块，将调用前的部分连接到克隆函数体的入口，
 *        并将所有克隆函数体中的 `ret` 指令替换为指向调用点后半部分块的跳转。
 *     d. **返回值处理**：如果被调用函数有返回值，则创建一个 PHI 指令来合并所有
 *        `ret` 指令返回的值，并将原始 `call` 指令的结果替换为该 PHI 节点。
 * 3.  **不动点迭代**：由于一次内联可能会产生新的内联机会，整个过程会反复执行，
 *     直到没有更多的内联可以进行为止。
 */
#include "ir/transforms/inliner.h"
#include "ast.h" // for pool_alloc, pool_strdup, TYP...
#include "ir/ir_builder.h"
#include "ir/ir_utils.h"
#include "ir/transforms/simplify_cfg.h"
#include "logger.h" // for LOG_CATEGORY_IR_OPT, LOG_DEBUG
#include <string.h>

// --- 配置与数据结构 ---

// 启发式规则：只有指令数小于此阈值的函数才会被考虑内联。
#define INLINE_THRESHOLD 80

/**
 * @brief 存储内联遍执行期间所需状态的上下文。
 */
typedef struct {
  IRModule *module;        ///< 当前正在处理的模块
  IRBuilder builder;       ///< 用于创建新指令的构建器
  Worklist *call_stack;    ///< 调用栈，用于检测和避免递归内联
  bool changed_this_round; ///< 标记当前一轮不动点迭代中是否发生了改变
} InlinerContext;

// 外部函数的前向声明
void ir_builder_set_insertion_block_end(IRBuilder *builder, IRBasicBlock *bb);
void ir_builder_set_insertion_block_start(IRBuilder *builder,
                                          IRBasicBlock *block);
IRInstruction *clone_instruction(IRInstruction *instr, MemoryPool *pool);
void append_instruction_to_block(IRBasicBlock *block, IRInstruction *instr);
IRBasicBlock *split_block_after_instruction(IRBuilder *builder,
                                            IRInstruction *instr);
void mark_instruction_for_removal(IRInstruction *instr);
void cleanup_removed_instructions(IRBasicBlock *block);
bool is_global_or_function(IRValue *val);
bool is_terminator(Opcode op);

// --- 静态函数声明 ---
static int count_instructions(IRFunction *func);
static bool find_and_inline_calls(InlinerContext *ctx);
static bool inline_call_site(InlinerContext *ctx, IRInstruction *call_instr);
static IRValue *get_callee_from_call(IRInstruction *call_instr);
static IRFunction *find_function_in_module(IRModule *module,
                                           const char *func_name);
static bool should_inline_function(InlinerContext *ctx, IRFunction *callee);
static void clone_and_remap_function_body(InlinerContext *ctx,
                                          IRInstruction *call_instr,
                                          IRFunction *callee,
                                          ValueMap *val_map);
static void connect_cfg_after_inlining(InlinerContext *ctx,
                                       IRInstruction *call_instr,
                                       IRFunction *callee, ValueMap *val_map);

// --- 主入口函数 ---
bool run_inliner(IRFunction *func) {
  if (!func || !func->entry) {
    if (func && func->module && func->module->log_config) {
      LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT,
                "Inliner: No function or entry block");
    }
    return false;
  }
  bool changed_overall = false;
  InlinerContext ctx = {.module = func->module,
                        .call_stack = create_worklist(func->module->pool, 16)};
  ir_builder_init(&ctx.builder, NULL); // Builder 将在每个函数内联时重新初始化

  // 使用不动点迭代，因为一次内联可能暴露新的内联机会
  while (true) {
    ctx.changed_this_round = false;
    find_and_inline_calls(&ctx);
    if (ctx.changed_this_round) {
      changed_overall = true;
      // 每轮内联后，运行 CFG 简化来清理可能产生的冗余块和跳转
      for (IRFunction *f = func->module->functions; f; f = f->next) {
        if (f->entry == NULL)
          continue;
        run_simplify_cfg(f);
      }
    } else {
      break; // 达到不动点，退出循环
    }
  }

  if (changed_overall && func->module && func->module->log_config) {
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT,
              "Inliner pass completed with changes.");
  }
  return changed_overall;
}

// 在模块中查找所有可内联的调用点。
static bool find_and_inline_calls(InlinerContext *ctx) {
  Worklist *calls_to_inline = create_worklist(ctx->module->pool, 32);

  for (IRFunction *func = ctx->module->functions; func; func = func->next) {
    if (func->entry == NULL)
      continue;

    worklist_add(ctx->call_stack, func); // 将当前函数压入调用栈以检测递归
    for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
      for (IRInstruction *instr = bb->head; instr; instr = instr->next) {
        if (instr->opcode == IR_OP_CALL) {
          IRValue *callee_val = get_callee_from_call(instr);
          // 类型安全的函数查找：确保被调用者是一个函数，而不是间接调用
          if (callee_val && callee_val->is_global && callee_val->name) {
            IRFunction *callee_func =
                find_function_in_module(ctx->module, callee_val->name);
            if (callee_func && should_inline_function(ctx, callee_func)) {
              worklist_add(calls_to_inline, instr);
            }
          }
        }
      }
    }
    worklist_pop(ctx->call_stack); // 将当前函数弹出调用栈
  }

  if (calls_to_inline->count > 0) {
    for (int i = 0; i < calls_to_inline->count; ++i) {
      inline_call_site(ctx, (IRInstruction *)calls_to_inline->items[i]);
    }
    ctx->changed_this_round = true;
  }
  return ctx->changed_this_round;
}

// 对单个调用点执行内联操作。
static bool inline_call_site(InlinerContext *ctx, IRInstruction *call_instr) {
  IRBasicBlock *call_block = call_instr->parent;
  IRFunction *caller = call_block->parent;
  IRValue *callee_val = get_callee_from_call(call_instr);

  // 类型安全的函数查找
  if (!callee_val || !callee_val->is_global || !callee_val->name) {
    if (ctx->module && ctx->module->log_config) {
      LOG_DEBUG(ctx->module->log_config, LOG_CATEGORY_IR_OPT,
                "Inliner: Cannot inline indirect call");
    }
    return false;
  }

  IRFunction *callee = find_function_in_module(ctx->module, callee_val->name);
  if (!callee) {
    if (ctx->module && ctx->module->log_config) {
      LOG_DEBUG(ctx->module->log_config, LOG_CATEGORY_IR_OPT,
                "Inliner: Cannot find function %s", callee_val->name);
    }
    return false;
  }

  if (ctx->module && ctx->module->log_config) {
    LOG_DEBUG(ctx->module->log_config, LOG_CATEGORY_IR_OPT,
              "Inliner: Inlining call to @%s in @%s", callee->name,
              caller->name);
  }

  ir_builder_init(&ctx->builder, caller);
  ValueMap val_map;
  value_map_init(&val_map, ctx->module->pool);

  // 1. 克隆被调用函数的函数体，并重映射参数和指令
  clone_and_remap_function_body(ctx, call_instr, callee, &val_map);

  // 2. 切分调用点所在的基本块，并重新连接控制流图
  connect_cfg_after_inlining(ctx, call_instr, callee, &val_map);

  return true;
}

// --- 辅助函数实现 ---

// 从 call 指令中获取被调用函数。
static IRValue *get_callee_from_call(IRInstruction *call_instr) {
  if (!call_instr || !call_instr->operand_head)
    return NULL;
  return call_instr->operand_head->data.value;
}

// 类型安全的函数查找：通过函数名在模块中查找对应的 IRFunction
static IRFunction *find_function_in_module(IRModule *module,
                                           const char *func_name) {
  if (!module || !func_name)
    return NULL;

  for (IRFunction *func = module->functions; func; func = func->next) {
    if (func->name && strcmp(func->name, func_name) == 0) {
      return func;
    }
  }
  return NULL;
}

// 根据启发式规则判断一个函数是否应该被内联。
static bool should_inline_function(InlinerContext *ctx, IRFunction *callee) {
  if (!callee || callee->entry == NULL)
    return false; // 不能内联外部函数或没有定义的函数

  // 启发式规则 1：不内联递归调用
  for (int i = 0; i < ctx->call_stack->count; ++i) {
    if (ctx->call_stack->items[i] == callee) {
      return false;
    }
  }

  // 启发式规则 2：只内联"小"函数
  return count_instructions(callee) <= INLINE_THRESHOLD;
}

// 克隆被调用函数的函数体，并建立值的映射关系。
static void clone_and_remap_function_body(InlinerContext *ctx,
                                          IRInstruction *call_instr,
                                          IRFunction *callee,
                                          ValueMap *val_map) {
  // 记录函数内联操作
  LOG_DEBUG(ctx->module->log_config, LOG_CATEGORY_IR_OPT,
            "Inlining function %s with %d basic blocks", callee->name,
            callee->block_count);

  // 步骤 A: 将被调用者的旧基本块映射到新创建的克隆块
  for (IRBasicBlock *old_bb = callee->blocks; old_bb;
       old_bb = old_bb->next_in_func) {
    IRBasicBlock *new_bb =
        ir_builder_create_block(&ctx->builder, old_bb->label);

    // 检查ValueMap是否需要扩容（在扩容前记录日志）
    if (val_map->count >= val_map->capacity) {
      LOG_DEBUG(ctx->module->log_config, LOG_CATEGORY_MEMORY,
                "ValueMap expanding from capacity %d during basic block "
                "mapping in inliner",
                val_map->capacity);
    }

    value_map_put(val_map, (IRValue *)old_bb, (IRValue *)new_bb, ctx->module->log_config);
  }

  // --- 步骤 B (改进的 SSA 化参数处理) ---
  // 直接建立参数值的映射关系，而不是重新创建 alloca 和 store
  IROperand *arg_op = call_instr->operand_head->next_in_instr;
  for (int i = 0; i < callee->num_args; ++i) {
    IRValue *formal_arg = callee->args[i];    // 被调用者的参数 (e.g., %arg0)
    IRValue *actual_arg = arg_op->data.value; // 调用者传入的实参 (e.g., %5)

    // 直接建立映射: %arg0 -> %5
    value_map_put(val_map, formal_arg, actual_arg, ctx->module->log_config);

    arg_op = arg_op->next_in_instr;
  }

  // 步骤 C-1: 克隆所有指令（包括 PHI 和终结符），并建立值的映射
  for (IRBasicBlock *old_bb = callee->blocks; old_bb;
       old_bb = old_bb->next_in_func) {
    IRBasicBlock *new_bb =
        (IRBasicBlock *)value_map_get(val_map, (IRValue *)old_bb, ctx->module->log_config);
    for (IRInstruction *old_instr = old_bb->head; old_instr;
         old_instr = old_instr->next) {
      IRInstruction *new_instr =
          clone_instruction(old_instr, ctx->module->pool);
      append_instruction_to_block(new_bb, new_instr);
      if (old_instr->dest) {
        new_instr->dest = ir_builder_create_reg(
            &ctx->builder, old_instr->dest->type, old_instr->dest->name);
        value_map_put(val_map, old_instr->dest, new_instr->dest, ctx->module->log_config);
      }
    }
  }
  // 步骤 C-2: 重映射所有新指令的操作数
  for (IRBasicBlock *old_bb = callee->blocks; old_bb;
       old_bb = old_bb->next_in_func) {
    IRBasicBlock *new_bb =
        (IRBasicBlock *)value_map_get(val_map, (IRValue *)old_bb, ctx->module->log_config);
    for (IRInstruction *new_instr = new_bb->head; new_instr;
         new_instr = new_instr->next) {
      for (IROperand *op = new_instr->operand_head; op;
           op = op->next_in_instr) {
        if (op->kind == IR_OP_KIND_VALUE) {
          op->data.value = remap_value(val_map, op->data.value);
        } else if (op->kind == IR_OP_KIND_BASIC_BLOCK) {
          op->data.bb =
              (IRBasicBlock *)remap_value(val_map, (IRValue *)op->data.bb);
        }
      }
    }
  }
}

// 在内联后，重新连接控制流图。
static void connect_cfg_after_inlining(InlinerContext *ctx,
                                       IRInstruction *call_instr,
                                       IRFunction *callee, ValueMap *val_map) {
  IRBasicBlock *call_block = call_instr->parent;
  IRBasicBlock *entry_clone =
      (IRBasicBlock *)value_map_get(val_map, (IRValue *)callee->entry, ctx->module->log_config);

  // 1. 在调用指令后切分基本块，得到 call_block 和 after_call_block
  IRBasicBlock *after_call_block =
      split_block_after_instruction(&ctx->builder, call_instr);
  after_call_block->label = pool_strdup(ctx->module->pool, "inline.cont");

  // 2. 将原始调用块的终结符重定向到克隆函数体的入口
  redirect_edge(call_block, after_call_block, entry_clone);

  // 3. 如果有返回值，在 after_call_block 的开头创建一个 PHI
  // 指令来收集所有返回路径的值
  IRInstruction *ret_phi = NULL;
  if (call_instr->dest && call_instr->dest->type->kind != TYPE_VOID) {
    ir_builder_set_insertion_block_start(&ctx->builder, after_call_block);
    ret_phi = ir_builder_create_phi(&ctx->builder, call_instr->dest->type,
                                    "inline.ret");
  }

  // 4. 遍历克隆后的块，将所有 `ret` 指令替换为到 after_call_block 的无条件跳转
  for (IRBasicBlock *old_bb = callee->blocks; old_bb;
       old_bb = old_bb->next_in_func) {
    IRBasicBlock *new_bb =
        (IRBasicBlock *)value_map_get(val_map, (IRValue *)old_bb, ctx->module->log_config);
    if (new_bb->tail && new_bb->tail->opcode == IR_OP_RET) {
      IRInstruction *ret_instr = new_bb->tail;
      if (ret_phi && ret_instr->num_operands > 0) {
        IRValue *ret_val =
            remap_value(val_map, ret_instr->operand_head->data.value);
        ir_phi_add_incoming(ret_phi, ret_val, new_bb);
      }
      erase_instruction(ret_instr);
      ir_builder_set_insertion_block_end(&ctx->builder, new_bb);
      ir_builder_create_br(&ctx->builder, after_call_block);
    }
  }

  // 5. 最终清理
  if (ret_phi) {
    replace_all_uses_with(NULL, call_instr->dest, ret_phi->dest);
  }
  mark_instruction_for_removal(call_instr);
  cleanup_removed_instructions(call_block);
}

// 辅助函数：计算一个函数中的指令数量。
static int count_instructions(IRFunction *func) {
  int count = 0;
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    for (IRInstruction *instr = bb->head; instr; instr = instr->next) {
      count++;
    }
  }
  return count;
}

/**
 * @brief 在指定指令后切分基本块。
 * @param builder IR构建器。
 * @param instr 切分点指令。
 * @return 新创建的基本块，包含instr之后的所有指令。
 */
IRBasicBlock *split_block_after_instruction(IRBuilder *builder,
                                            IRInstruction *instr) {
  if (!instr || !instr->parent)
    return NULL;

  IRBasicBlock *old_block = instr->parent;
  IRFunction *func = old_block->parent;

  // 创建新的基本块
  IRBasicBlock *new_block = ir_builder_create_block(builder, "split");
  link_basic_block_to_function(new_block, func);

  // 找到instr之后的所有指令
  IRInstruction *first_moved_instr = instr->next;
  if (!first_moved_instr) {
    // 如果instr是最后一条指令，新块为空
    return new_block;
  }

  // 移动指令到新块
  IRInstruction *last_moved_instr = old_block->tail;

  // 更新旧块的尾指针
  old_block->tail = instr;
  instr->next = NULL;

  // 更新新块的头尾指针
  new_block->head = first_moved_instr;
  new_block->tail = last_moved_instr;

  // 更新所有移动指令的父块指针
  for (IRInstruction *moved_instr = first_moved_instr; moved_instr;
       moved_instr = moved_instr->next) {
    moved_instr->parent = new_block;
  }

  // 更新新块中指令的链表指针
  if (last_moved_instr) {
    last_moved_instr->next = NULL;
  }

  // 在旧块末尾添加无条件跳转到新块
  ir_builder_set_insertion_block_end(builder, old_block);
  ir_builder_create_br(builder, new_block);

  // 更新CFG关系
  // 1. 暂存旧块的原始后继（这些后继现在属于new_block的终结符）
  int num_old_succs = old_block->num_successors;
  IRBasicBlock **old_succs = NULL;
  if (num_old_succs > 0) {
    old_succs = (IRBasicBlock **)pool_alloc(
        func->module->pool, num_old_succs * sizeof(IRBasicBlock *));
    for (int i = 0; i < num_old_succs; ++i) {
      old_succs[i] = old_block->successors[i];
    }
  }

  // 2. 断开旧块与所有原始后继的连接
  for (int i = 0; i < num_old_succs; ++i) {
    remove_predecessor(old_succs[i], old_block);
  }
  old_block->num_successors = 0;

  // 3. 将 new_block 设置为 old_block 的唯一后继
  add_successor(old_block, new_block);
  add_predecessor(new_block, old_block);

  // 4. 将原始后继转移给 new_block（因为终结符现在在new_block中）
  for (int i = 0; i < num_old_succs; ++i) {
    add_successor(new_block, old_succs[i]);
    add_predecessor(old_succs[i], new_block);
  }

  return new_block;
}

/**
 * @brief 将指令追加到基本块末尾。
 * @param block 目标基本块。
 * @param instr 要追加的指令。
 */
void append_instruction_to_block(IRBasicBlock *block, IRInstruction *instr) {
  if (!block || !instr)
    return;

  // 设置指令的父块
  instr->parent = block;

  // 如果是第一条指令
  if (!block->head) {
    block->head = instr;
    block->tail = instr;
    instr->prev = NULL;
    instr->next = NULL;
    return;
  }

  // 追加到末尾
  instr->prev = block->tail;
  instr->next = NULL;
  block->tail->next = instr;
  block->tail = instr;
}