/**
 * @file ir_builder.c
 * @brief 实现 IRBuilder 辅助类，用于以编程方式构建IR。
 * @details
 * - 将所有底层的 IR 操作委托给 ir_utils 模块。
 * - 为所有指令类型提供了一套全面且一致的创建API。
 * - 管理用于命名和指令插入点的上下文状态。
 */
#include "ir/ir_builder.h"
#include "ast.h"
#include "ir/ir_utils.h"
#include <assert.h>
#include <stdbool.h> // for false, true
#include <stdio.h>
#include <string.h>

// --- Builder 生命周期管理 ---

/**
 * @brief 初始化一个 IRBuilder 实例。
 * @details
 * 将 Builder 与一个特定的函数关联起来，并设置默认的插入点
 * （通常是函数入口块的末尾）。
 *
 * @param builder 指向要初始化的 IRBuilder 的指针。
 * @param func Builder 将要操作的函数。
 */
void ir_builder_init(IRBuilder *builder, IRFunction *func) {
  assert(builder && func && "Builder and function must not be null.");
  memset(builder, 0, sizeof(IRBuilder));
  builder->module = func->module;
  builder->current_func = func;

  // 默认情况下，将插入点设置在入口块的末尾（如果存在）。
  if (func->entry) {
    builder->current_bb = func->entry;
    builder->insert_point = NULL;
  }

  // 启发式地初始化计数器，以避免与IR生成器中的命名冲突。
  // 更健壮的系统可能会扫描函数以获取最大计数值。
  builder->temp_reg_count = func->block_count * 15;
  builder->label_count = func->block_count;
}

/**
 * @brief 设置 Builder 的插入点到某条指令之前。
 * @param builder IRBuilder 实例。
 * @param instr 新指令将被插入到此指令之前。
 */
void ir_builder_set_insertion_point(IRBuilder *builder, IRInstruction *instr) {
  assert(instr && instr->parent &&
         "Insertion point instruction must be valid and in a basic block.");
  builder->current_bb = instr->parent;
  builder->insert_point = instr;
}

/**
 * @brief 设置 Builder 的插入点到某个基本块的末尾。
 * @param builder IRBuilder 实例。
 * @param bb 目标基本块。
 */
void ir_builder_set_insertion_block_end(IRBuilder *builder, IRBasicBlock *bb) {
  assert(bb && "Insertion block must be valid.");
  builder->current_bb = bb;
  builder->insert_point = NULL; // NULL 插入点意味着插入到末尾。
}

/**
 * @brief 设置 Builder 的插入点到某个基本块的开头。
 * @details
 * 这个函数特别适用于插入PHI节点，因为PHI节点必须放在基本块的最开始。
 * 插入点被设置为NULL，这样新指令会被插入到块的第一条指令之前。
 *
 * @param builder IRBuilder 实例。
 * @param bb 目标基本块。
 */
void ir_builder_set_insertion_block_start(IRBuilder *builder,
                                          IRBasicBlock *bb) {
  assert(bb && "Insertion block must be valid.");
  builder->current_bb = bb;
  builder->insert_point = bb->head; // 设置到块的第一条指令之前
}

// 在合适位置添加实现
void ir_builder_set_insertion_block(IRBuilder *builder, IRBasicBlock *bb) {
  assert(builder && bb && "Builder and block must not be null.");
  builder->current_bb = bb;
  builder->insert_point = NULL;
}

// --- 内部辅助函数，用于在当前 Builder 位置插入指令 ---
static void insert_instruction_at_point(IRBuilder *builder,
                                        IRInstruction *instr) {
  assert(builder->current_bb &&
         "Builder does not have a valid insertion block.");
  if (builder->insert_point) {
    insert_instr_before(instr, builder->insert_point);
  } else {
    // 在基本块的末尾（但在终结符之前）插入
    add_instr_to_bb_end(builder->current_bb, instr);
  }
}

// --- IR 创建 API 实现 ---

// --- 通用的二元操作创建函数 ---
static IRInstruction *create_binary_op(IRBuilder *builder, Opcode op,
                                       IRValue *lhs, IRValue *rhs,
                                       const char *name) {
  // 记录二元操作指令创建
  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating binary operation %d in block %s", op,
            builder->current_bb ? builder->current_bb->label : "unknown");

  IRInstruction *instr = create_ir_instruction(op, builder->module->pool);
  // 二元操作的结果类型通常与操作数类型相同。
  instr->dest = ir_builder_create_reg(builder, lhs->type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, lhs);
  add_value_operand(instr, rhs);
  insert_instruction_at_point(builder, instr);
  return instr;
}

// --- 整数算术 ---
IRInstruction *ir_builder_create_add(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_ADD, lhs, rhs, name);
}
IRInstruction *ir_builder_create_sub(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_SUB, lhs, rhs, name);
}
IRInstruction *ir_builder_create_mul(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_MUL, lhs, rhs, name);
}
IRInstruction *ir_builder_create_sdiv(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_SDIV, lhs, rhs, name);
}
IRInstruction *ir_builder_create_srem(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_SREM, lhs, rhs, name);
}

// --- 位运算 ---
IRInstruction *ir_builder_create_shl(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_SHL, lhs, rhs, name);
}
IRInstruction *ir_builder_create_ashr(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_ASHR, lhs, rhs, name);
}
IRInstruction *ir_builder_create_and(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_AND, lhs, rhs, name);
}
IRInstruction *ir_builder_create_or(IRBuilder *builder, IRValue *lhs,
                                    IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_OR, lhs, rhs, name);
}
IRInstruction *ir_builder_create_xor(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_XOR, lhs, rhs, name);
}

// --- 浮点算术 ---
IRInstruction *ir_builder_create_fadd(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_FADD, lhs, rhs, name);
}
IRInstruction *ir_builder_create_fsub(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_FSUB, lhs, rhs, name);
}
IRInstruction *ir_builder_create_fmul(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_FMUL, lhs, rhs, name);
}
IRInstruction *ir_builder_create_fdiv(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name) {
  return create_binary_op(builder, IR_OP_FDIV, lhs, rhs, name);
}

// --- 内存操作 ---
IRInstruction *ir_builder_create_alloca(IRBuilder *builder, Type *type,
                                        const char *name) {
  // 记录alloca指令创建
  LOG_DEBUG(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating ALLOCA instruction for type in function %s",
            builder->current_func ? builder->current_func->name : "unknown");

  IRInstruction *instr =
      create_ir_instruction(IR_OP_ALLOCA, builder->module->pool);
  // `alloca T` 指令的结果是一个指向T的指针，即 `T*`。
  Type *ptr_type = create_pointer_type(type, false, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, ptr_type, name);
  instr->dest->def_instr = instr;
  // 根据类型计算适当的内存对齐
  int align = 4; // 默认对齐
  if (type->kind == TYPE_BASIC) {
    switch (type->basic) {
    case BASIC_I1:
    case BASIC_I8:
      align = 1;
      break;
    case BASIC_INT:
      align = 4;
      break;
    case BASIC_FLOAT:
      align = 4;
      break;
    case BASIC_DOUBLE:
      align = 8;
      break;
    default:
      align = 4;
      break;
    }
  } else if (type->kind == TYPE_ARRAY) {
    // 数组对齐与元素类型对齐相同
    Type *elem_type = type->array.element_type;
    if (elem_type->kind == TYPE_BASIC) {
      switch (elem_type->basic) {
      case BASIC_DOUBLE:
        align = 8;
        break;
      case BASIC_FLOAT:
      case BASIC_INT:
        align = 4;
        break;
      default:
        align = 4;
        break;
      }
    }
  }
  instr->align = align;

  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_MEMORY,
            "ALLOCA instruction aligned to %d bytes", align);

  // `alloca` 指令必须插入到函数入口块的顶部。
  IRBasicBlock *entry_block = builder->current_func->entry;
  IRInstruction *first_non_alloca = entry_block->head;
  while (first_non_alloca && first_non_alloca->opcode == IR_OP_ALLOCA) {
    first_non_alloca = first_non_alloca->next;
  }

  if (first_non_alloca) {
    insert_instr_before(instr, first_non_alloca);
  } else {
    add_instr_to_bb_end(entry_block, instr);
  }
  return instr;
}

IRInstruction *ir_builder_create_load(IRBuilder *builder, IRValue *ptr,
                                      const char *name) {
  assert(ptr->type->kind == TYPE_POINTER && "Can only load from a pointer.");

  // 记录load指令创建
  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating LOAD instruction from pointer in block %s",
            builder->current_bb ? builder->current_bb->label : "unknown");

  IRInstruction *instr =
      create_ir_instruction(IR_OP_LOAD, builder->module->pool);
  instr->dest =
      ir_builder_create_reg(builder, ptr->type->pointer.element_type, name);
  instr->dest->def_instr = instr;
  instr->align = 4;
  add_value_operand(instr, ptr);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_store(IRBuilder *builder, IRValue *val,
                                       IRValue *ptr) {
  assert(ptr->type->kind == TYPE_POINTER && "Store address must be a pointer.");

  // 记录store指令创建
  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating STORE instruction to pointer in block %s",
            builder->current_bb ? builder->current_bb->label : "unknown");

  IRInstruction *instr =
      create_ir_instruction(IR_OP_STORE, builder->module->pool);
  instr->align = 4;
  add_value_operand(instr, val);
  add_value_operand(instr, ptr);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_gep(IRBuilder *builder, IRValue *ptr,
                                     IRValue **indices, int num_indices,
                                     const char *name) {
  assert(ptr->type->kind == TYPE_POINTER || ptr->type->kind == TYPE_ARRAY);
  IRInstruction *instr =
      create_ir_instruction(IR_OP_GETELEMENTPTR, builder->module->pool);

  // 记录GEP指令创建
  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating GEP instruction with %d indices", num_indices);

  // 改进的GEP类型计算，正确处理多维数组
  Type *current_type = ptr->type;

  // 第一个索引总是用于指针解引用（如果是指针类型）
  if (current_type->kind == TYPE_POINTER) {
    current_type = current_type->pointer.element_type;
  }

  // 处理剩余的索引，每个索引都会进入下一层维度
  for (int i = 1; i < num_indices; ++i) {
    if (current_type->kind == TYPE_ARRAY) {
      current_type = current_type->array.element_type;
    } else {
      // 如果索引数量超过了数组维度，记录警告
      LOG_WARN(builder->module->log_config, LOG_CATEGORY_IR_GEN,
               "GEP index count exceeds array dimensions");
      break;
    }
  }

  // 结果类型是指向最终元素类型的指针
  Type *result_type =
      create_pointer_type(current_type, false, builder->module->pool);

  instr->dest = ir_builder_create_reg(builder, result_type, name);
  instr->dest->def_instr = instr;
  instr->is_inbounds = true; // 为简单起见，默认为 inbounds

  add_value_operand(instr, ptr);
  for (int i = 0; i < num_indices; ++i) {
    add_value_operand(instr, indices[i]);
  }

  insert_instruction_at_point(builder, instr);
  return instr;
}

// --- 比较、转换和其他指令 ---

IRInstruction *ir_builder_create_icmp(IRBuilder *builder, const char *cond,
                                      IRValue *lhs, IRValue *rhs,
                                      const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_ICMP, builder->module->pool);
  instr->opcode_cond = pool_strdup(builder->module->pool, cond);
  // ICMP 的结果类型为 i1
  instr->dest = ir_builder_create_reg(
      builder, create_basic_type(BASIC_I1, false, builder->module->pool), name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, lhs);
  add_value_operand(instr, rhs);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_fcmp(IRBuilder *builder, const char *cond,
                                      IRValue *lhs, IRValue *rhs,
                                      const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_FCMP, builder->module->pool);
  instr->opcode_cond = pool_strdup(builder->module->pool, cond);
  instr->dest = ir_builder_create_reg(
      builder, create_basic_type(BASIC_I1, false, builder->module->pool), name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, lhs);
  add_value_operand(instr, rhs);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_zext(IRBuilder *builder, IRValue *operand,
                                      Type *dest_type, const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_ZEXT, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, operand);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_fpext(IRBuilder *builder, IRValue *operand,
                                       Type *dest_type, const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_FPEXT, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, operand);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_sitofp(IRBuilder *builder, IRValue *operand,
                                        Type *dest_type, const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_SITOFP, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, operand);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_fptosi(IRBuilder *builder, IRValue *operand,
                                        Type *dest_type, const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_FPTOSI, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, operand);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_phi(IRBuilder *builder, Type *type,
                                     const char *name) {
  // 记录PHI指令创建
  LOG_DEBUG(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating PHI instruction in block %s",
            builder->current_bb ? builder->current_bb->label : "unknown");

  IRInstruction *instr =
      create_ir_instruction(IR_OP_PHI, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, type, name);
  instr->dest->def_instr = instr;

  // PHI 节点必须放在基本块的开头。
  IRInstruction *first_non_phi = builder->current_bb->head;
  while (first_non_phi && first_non_phi->opcode == IR_OP_PHI) {
    first_non_phi = first_non_phi->next;
  }

  if (first_non_phi) {
    insert_instr_before(instr, first_non_phi);
  } else {
    add_instr_to_bb_end(builder->current_bb, instr);
  }
  return instr;
}

/**
 * @brief 向一个PHI指令添加入口（incoming）值。
 * @param phi 目标PHI指令。
 * @param val 来自前驱基本块的值。
 * @param pred 对应的前驱基本块。
 */
void ir_phi_add_incoming(IRInstruction *phi, IRValue *val, IRBasicBlock *pred) {
  assert(phi && phi->opcode == IR_OP_PHI &&
         "Can only add incoming to a PHI node.");
  assert(val && "PHI incoming value cannot be NULL.");
  assert(pred && "PHI incoming block cannot be NULL.");

  // 验证类型兼容性：PHI的所有入口值必须与结果类型一致
  if (phi->dest && phi->dest->type && val->type) {
    if (!is_type_same(phi->dest->type, val->type, true)) {
      // 类型不匹配，记录警告
      if (phi->parent && phi->parent->parent && phi->parent->parent->module &&
          phi->parent->parent->module->log_config) {
        LOG_WARN(phi->parent->parent->module->log_config, LOG_CATEGORY_IR_GEN,
                 "PHI type mismatch: expected %s, got %s in block %s",
                 phi->dest->type->kind == TYPE_BASIC ? "basic_type"
                                                     : "complex_type",
                 val->type->kind == TYPE_BASIC ? "basic_type" : "complex_type",
                 phi->parent->label ? phi->parent->label : "unnamed");
      }
    }
  }

  // 检查是否已经存在来自同一前驱的入口（避免重复）
  for (IROperand *op = phi->operand_head; op; op = op->next_in_instr) {
    if (op->kind == IR_OP_KIND_BASIC_BLOCK && op->data.bb == pred) {
      // 已经存在来自该前驱的入口，不应该重复添加
      assert(false &&
             "PHI node already has an incoming value from this predecessor.");
      return;
    }
  }

  add_value_operand(phi, val);
  add_bb_operand(phi, pred);
}

IRInstruction *ir_builder_create_call(IRBuilder *builder, IRValue *func_val,
                                      IRValue **args, int num_args,
                                      const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_CALL, builder->module->pool);
  add_value_operand(instr, func_val);
  for (int i = 0; i < num_args; ++i) {
    add_value_operand(instr, args[i]);
  }
  // 新增：为有返回值的 call 指令分配目的寄存器
  Type *func_type = func_val->type->pointer.element_type;
  if (func_type->function.return_type->kind != TYPE_VOID) {
    instr->dest = ir_builder_create_reg(
        builder, func_type->function.return_type, name ? name : "calltmp");
    instr->dest->def_instr = instr;
  }
  insert_instruction_at_point(builder, instr);
  return instr;
}

// --- 终结符指令 ---

IRInstruction *ir_builder_create_br(IRBuilder *builder, IRBasicBlock *dest) {
  assert(builder->current_bb->tail == NULL &&
         "Block already has a terminator.");
  IRInstruction *instr = create_ir_instruction(IR_OP_BR, builder->module->pool);
  add_bb_operand(instr, dest);
  add_instr_to_bb_end(builder->current_bb, instr);
  return instr;
}

IRInstruction *ir_builder_create_cond_br(IRBuilder *builder, IRValue *cond,
                                         IRBasicBlock *true_dest,
                                         IRBasicBlock *false_dest) {
  assert(builder->current_bb->tail == NULL &&
         "Block already has a terminator.");
  IRInstruction *instr = create_ir_instruction(IR_OP_BR, builder->module->pool);
  add_value_operand(instr, cond);
  add_bb_operand(instr, true_dest);
  add_bb_operand(instr, false_dest);
  add_instr_to_bb_end(builder->current_bb, instr);
  return instr;
}

IRInstruction *ir_builder_create_ret(IRBuilder *builder, IRValue *val) {
  assert(builder->current_bb->tail == NULL &&
         "Block already has a terminator.");
  IRInstruction *instr =
      create_ir_instruction(IR_OP_RET, builder->module->pool);
  if (val)
    add_value_operand(instr, val);
  add_instr_to_bb_end(builder->current_bb, instr);
  return instr;
}

IRInstruction *ir_builder_create_ret_void(IRBuilder *builder) {
  return ir_builder_create_ret(builder, NULL);
}

// --- 值和基本块的创建 ---

/**
 * @brief 创建一个新的、唯一的虚拟寄存器。
 * @param builder IRBuilder 实例。
 * @param type 寄存器的类型。
 * @param name_prefix 寄存器名称的前缀，用于提高可读性。
 * @return 新创建的 IRValue。
 */
IRValue *ir_builder_create_reg(IRBuilder *builder, Type *type,
                               const char *name_prefix) {
  IRValue *val =
      (IRValue *)pool_alloc_z(builder->module->pool, sizeof(IRValue));
  val->is_constant = false;
  val->type = type;
  char *reg_name = (char *)pool_alloc(
      builder->module->pool, strlen(name_prefix ? name_prefix : "tmp") + 15);
  sprintf(reg_name, "%s.%d", name_prefix ? name_prefix : "tmp",
          builder->temp_reg_count++);
  val->name = reg_name;
  return val;
}

IRValue *ir_builder_create_const_int(IRBuilder *builder, int value) {
  IRValue *val =
      (IRValue *)pool_alloc_z(builder->module->pool, sizeof(IRValue));
  val->is_constant = true;
  val->type = create_basic_type(BASIC_INT, true, builder->module->pool);
  val->int_val = value;
  return val;
}

IRValue *ir_builder_create_const_float(IRBuilder *builder, float value) {
  IRValue *val =
      (IRValue *)pool_alloc_z(builder->module->pool, sizeof(IRValue));
  val->is_constant = true;
  val->type = create_basic_type(BASIC_FLOAT, true, builder->module->pool);
  val->float_val = value;
  return val;
}

IRValue *ir_builder_create_const_double(IRBuilder *builder, double value) {
  return create_constant_double(value, builder->module->pool);
}

/**
 * @brief 创建一个新的基本块。
 * @details 会自动为块生成一个唯一的标签，并将其添加到当前函数中。
 * @param builder IRBuilder 实例。
 * @param name_prefix 块标签的前缀。
 * @return 新创建的 IRBasicBlock。
 */
IRBasicBlock *ir_builder_create_block(IRBuilder *builder,
                                      const char *name_prefix) {
  char label_buf[128];
  snprintf(label_buf, sizeof(label_buf), "%s.%d", name_prefix,
           builder->label_count++);
  // 调用 ir_data_structures.h 中定义的底层创建函数。
  return create_ir_basic_block(label_buf, builder->current_func,
                               builder->module->pool);
}

IRInstruction *ir_builder_create_sext(IRBuilder *builder, IRValue *src,
                                      Type *dest_type, const char *name) {
  IRInstruction *instr =
      create_ir_instruction(IR_OP_SEXT, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, src);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_trunc(IRBuilder *builder, IRValue *src,
                                       Type *dest_type, const char *name) {
  // 记录类型转换指令创建
  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating TRUNC instruction: %s -> %s",
            src->type ? "source_type" : "unknown",
            dest_type ? "dest_type" : "unknown");

  IRInstruction *instr =
      create_ir_instruction(IR_OP_TRUNC, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, src);
  insert_instruction_at_point(builder, instr);
  return instr;
}

IRInstruction *ir_builder_create_fptrunc(IRBuilder *builder, IRValue *src,
                                         Type *dest_type, const char *name) {
  // 记录浮点截断指令创建
  LOG_TRACE(builder->module->log_config, LOG_CATEGORY_IR_GEN,
            "Creating FPTRUNC instruction: double -> float");

  IRInstruction *instr =
      create_ir_instruction(IR_OP_FPTRUNC, builder->module->pool);
  instr->dest = ir_builder_create_reg(builder, dest_type, name);
  instr->dest->def_instr = instr;
  add_value_operand(instr, src);
  insert_instruction_at_point(builder, instr);
  return instr;
}