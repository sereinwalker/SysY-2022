#include "ir/ir_data_structures.h"
#include "ir/ir_utils.h"

/**
 * @file ir_data_structures.c
 * @brief 实现核心 IR 数据结构的创建和管理函数。
 *
 * @details
 * 本文件提供了使用内存池分配来创建和管理 IR 对象的功能，
 * 以获得更好的性能和简化的内存管理。
 *
 * 注意：模块生命周期函数 (create_ir_module, destroy_ir_module)
 * 在 ir_lifecycle.c 中实现，以保持职责分离。
 */

// --- IR 对象链接函数 ---

/**
 * @brief 将一个函数链接到模块的函数列表中。
 * @param func 要链接的函数。
 * @param module 目标模块。
 */
void link_function_to_module(IRFunction *func, IRModule *module) {
  if (!func || !module)
    return;

  // 将函数添加到模块函数列表的头部。
  func->next = module->functions;
  module->functions = func;
}

/**
 * @brief 将一个基本块链接到函数的块列表中。
 * @param bb 要链接的基本块。
 * @param func 目标函数。
 */
void link_basic_block_to_function(IRBasicBlock *bb, IRFunction *func) {
  if (!bb || !func)
    return;

  // 将基本块添加到函数块双向列表的头部。
  bb->next_in_func = func->blocks;
  if (func->blocks) {
    func->blocks->prev_in_func = bb;
  }
  func->blocks = bb;

  // 注意：这里的安全检查是为了防御性编程。
  // 按照约定，函数的入口块应该是第一个被创建和链接的块，
  // 并且应该在 IR 生成器中显式设置 func->entry。
  // 这个检查提供了一个额外的安全网，以防约定被违反。
  if (!func->entry) {
    func->entry = bb;
  }
}

/**
 * @brief 将一条指令链接到基本块的指令列表中。
 * @param instr 要链接的指令。
 * @param bb 目标基本块。
 */
void link_instruction_to_basic_block(IRInstruction *instr, IRBasicBlock *bb) {
  if (!instr || !bb)
    return;

  // 将指令添加到基本块指令双向列表的头部。
  instr->next = bb->head;
  if (bb->head) {
    bb->head->prev = instr;
  }
  bb->head = instr;
  // 如果块为空，新指令既是头也是尾。
  if (!bb->tail) {
    bb->tail = instr;
  }
  instr->parent = bb;
}

/**
 * @brief 将一个全局变量链接到模块的全局变量列表中。
 * @param global 要链接的全局变量。
 * @param module 目标模块。
 */
void link_global_to_module(IRGlobalVariable *global, IRModule *module) {
  if (!global || !module)
    return;

  // 将全局变量添加到模块全局变量列表的头部。
  global->next = module->globals;
  module->globals = global;
}

// --- IR 对象查询函数 ---

/**
 * @brief 检查一条指令是否为终结符指令。
 * @param instr 要检查的指令。
 * @return 如果是 RET 或 BR，返回 true。
 */
bool is_terminator_instruction(IRInstruction *instr) {
  if (!instr)
    return false;

  return instr->opcode == IR_OP_RET || instr->opcode == IR_OP_BR;
}

/**
 * @brief 检查一条指令是否为二元运算指令。
 * @param instr 要检查的指令。
 * @return 如果是，返回 true。
 */
bool is_binary_operation(IRInstruction *instr) {
  if (!instr)
    return false;

  switch (instr->opcode) {
  case IR_OP_ADD:
  case IR_OP_SUB:
  case IR_OP_MUL:
  case IR_OP_SDIV:
  case IR_OP_SREM:
  case IR_OP_FADD:
  case IR_OP_FSUB:
  case IR_OP_FMUL:
  case IR_OP_FDIV:
  case IR_OP_SHL:
  case IR_OP_LSHR:
  case IR_OP_ASHR:
  case IR_OP_AND:
  case IR_OP_OR:
  case IR_OP_XOR:
  case IR_OP_ICMP:
  case IR_OP_FCMP:
    return true;
  default:
    return false;
  }
}

/**
 * @brief 检查一条指令是否为一元运算（类型转换）指令。
 * @param instr 要检查的指令。
 * @return 如果是，返回 true。
 */
bool is_unary_operation(IRInstruction *instr) {
  if (!instr)
    return false;

  switch (instr->opcode) {
  case IR_OP_SITOFP:
  case IR_OP_FPTOSI:
  case IR_OP_ZEXT:
  case IR_OP_FPEXT:
    return true;
  default:
    return false;
  }
}

/**
 * @brief 检查一条指令是否为内存操作指令。
 * @param instr 要检查的指令。
 * @return 如果是，返回 true。
 */
bool is_memory_operation(IRInstruction *instr) {
  if (!instr)
    return false;

  switch (instr->opcode) {
  case IR_OP_ALLOCA:
  case IR_OP_LOAD:
  case IR_OP_STORE:
  case IR_OP_GETELEMENTPTR:
    return true;
  default:
    return false;
  }
}

/**
 * @brief 检查一条指令是否具有副作用。
 * @details 有副作用的指令不能被轻易地重排或删除，例如内存写入、函数调用等。
 * 注意：LOAD
 * 操作也被视为有副作用，因为它读取内存，在多线程环境或存在内存别名的情况下，
 * 其结果是不确定的，不能被随意重排或删除。
 * @param instr 要检查的指令。
 * @return 如果有副作用，返回 true。
 */
bool has_side_effects(IRInstruction *instr) {
  if (!instr)
    return false;

  // Instructions with side effects
  switch (instr->opcode) {
  case IR_OP_LOAD:  // 内存读取（在多线程或存在别名时不确定）
  case IR_OP_STORE: // 内存写入
  case IR_OP_CALL:  // 函数调用（可能具有副作用）
  case IR_OP_RET:   // 函数返回（控制流改变）
  case IR_OP_BR:    // 控制流改变
    return true;
  default:
    return false;
  }
}

// --- IR 对象验证函数 ---

/**
 * @brief 验证单条指令的合法性（主要是操作数数量）。
 * @param instr 要验证的指令。
 * @return 如果合法，返回 true。
 */
bool validate_instruction(IRInstruction *instr) {
  if (!instr)
    return false;

  // Basic validation
  if (instr->opcode == IR_OP_UNKNOWN)
    return false;

  // Validate operands based on opcode
  switch (instr->opcode) {
  case IR_OP_RET:
    // Return can have 0 or 1 operand
    return instr->num_operands <= 1;
  case IR_OP_BR:
    // Branch must have 1 or 3 operands (unconditional or conditional)
    return instr->num_operands == 1 || instr->num_operands == 3;
  case IR_OP_ADD:
  case IR_OP_SUB:
  case IR_OP_MUL:
  case IR_OP_SDIV:
  case IR_OP_SREM:
  case IR_OP_FADD:
  case IR_OP_FSUB:
  case IR_OP_FMUL:
  case IR_OP_FDIV:
  case IR_OP_ICMP:
  case IR_OP_FCMP:
    // Binary operations must have exactly 2 operands
    return instr->num_operands == 2;
  case IR_OP_ALLOCA:
    // Alloca has 0 operands - it only accepts a type, not a size operand
    return instr->num_operands == 0;
  case IR_OP_LOAD:
    // Load must have exactly 1 operand (pointer)
    return instr->num_operands == 1;
  case IR_OP_STORE:
    // Store must have exactly 2 operands (value, pointer)
    return instr->num_operands == 2;
  default:
    return true; // Other instructions are considered valid for now
  }
}

/**
 * @brief 验证单个基本块的合法性。
 * @details 检查块是否有标签、指令是否合法、是否以终结符指令结尾。
 * @param bb 要验证的基本块。
 * @return 如果合法，返回 true。
 */
bool validate_basic_block(IRBasicBlock *bb) {
  if (!bb)
    return false;

  // Basic block must have a label
  if (!bb->label)
    return false;

  // Validate all instructions in the block
  IRInstruction *instr = bb->head;
  while (instr) {
    if (!validate_instruction(instr))
      return false;
    instr = instr->next;
  }

  // Basic block must end with a terminator (except for unreachable blocks)
  if (bb->tail && !is_terminator_instruction(bb->tail)) {
    return false;
  }

  return true;
}

/**
 * @brief 验证单个函数的合法性。
 * @details 检查函数是否有名称、入口块，并验证其所有基本块。
 * @param func 要验证的函数。
 * @return 如果合法，返回 true。
 */
bool validate_function(IRFunction *func) {
  if (!func)
    return false;

  // Function must have a name
  if (!func->name)
    return false;

  // Function must have an entry block
  if (!func->entry)
    return false;

  // Validate all basic blocks in the function
  IRBasicBlock *bb = func->blocks;
  while (bb) {
    if (!validate_basic_block(bb))
      return false;
    bb = bb->next_in_func;
  }

  return true;
}