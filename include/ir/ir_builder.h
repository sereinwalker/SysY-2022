// --- START OF FILE ir/ir_builder.h ---

#ifndef IR_BUILDER_H
#define IR_BUILDER_H

#include "ir/ir_data_structures.h"

/**
 * @file ir_builder.h
 * @brief Defines a helper structure for programmatic IR construction.
 * The IRBuilder simplifies the process of creating and inserting IR
 * instructions, managing insertion points, and creating values like constants
 * and registers.
 */

typedef struct IRBuilder {
  IRModule *module;
  IRFunction *current_func;
  IRBasicBlock *current_bb;
  IRInstruction *insert_point; // Instruction to insert BEFORE. If NULL, insert
                               // at end of block.

  // Context for creating unique names
  int temp_reg_count;
  int label_count;

} IRBuilder;

// --- Builder Lifecycle ---
void ir_builder_init(IRBuilder *builder, IRFunction *func);
void ir_builder_set_insertion_point(IRBuilder *builder, IRInstruction *instr);
void ir_builder_set_insertion_block(IRBuilder *builder, IRBasicBlock *bb);
void ir_builder_set_insertion_block_end(IRBuilder *builder, IRBasicBlock *bb);
void ir_builder_set_insertion_block_start(IRBuilder *builder, IRBasicBlock *bb);

// --- IR Creation API ---

// Binary Instruction Creation
IRInstruction *ir_builder_create_add(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_fadd(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_sub(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_fsub(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_mul(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_fmul(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_sdiv(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_fdiv(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_srem(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_shl(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_lshr(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_ashr(IRBuilder *builder, IRValue *lhs,
                                      IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_and(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_or(IRBuilder *builder, IRValue *lhs,
                                    IRValue *rhs, const char *name);
IRInstruction *ir_builder_create_xor(IRBuilder *builder, IRValue *lhs,
                                     IRValue *rhs, const char *name);

// Unary Instruction Creation
IRInstruction *ir_builder_create_sitofp(IRBuilder *builder, IRValue *operand,
                                        Type *dest_type, const char *name);
IRInstruction *ir_builder_create_fptosi(IRBuilder *builder, IRValue *operand,
                                        Type *dest_type, const char *name);
IRInstruction *ir_builder_create_zext(IRBuilder *builder, IRValue *operand,
                                      Type *dest_type, const char *name);
IRInstruction *ir_builder_create_fpext(IRBuilder *builder, IRValue *operand,
                                       Type *dest_type, const char *name);
IRInstruction *ir_builder_create_fptrunc(IRBuilder *builder, IRValue *src,
                                         Type *dest_type, const char *name);

// Memory Instruction Creation
IRInstruction *ir_builder_create_alloca(IRBuilder *builder, Type *type,
                                        const char *name);
IRInstruction *ir_builder_create_load(IRBuilder *builder, IRValue *ptr,
                                      const char *name);
IRInstruction *ir_builder_create_store(IRBuilder *builder, IRValue *val,
                                       IRValue *ptr);
IRInstruction *ir_builder_create_gep(IRBuilder *builder, IRValue *ptr,
                                     IRValue **indices, int num_indices,
                                     const char *name);

// Comparison Instruction Creation
IRInstruction *ir_builder_create_icmp(IRBuilder *builder, const char *cond,
                                      IRValue *lhs, IRValue *rhs,
                                      const char *name);
IRInstruction *ir_builder_create_fcmp(IRBuilder *builder, const char *cond,
                                      IRValue *lhs, IRValue *rhs,
                                      const char *name);

// PHI Instruction Creation
IRInstruction *ir_builder_create_phi(IRBuilder *builder, Type *type,
                                     const char *name);
void ir_phi_add_incoming(IRInstruction *phi, IRValue *val, IRBasicBlock *pred);

// Function Call Creation
IRInstruction *ir_builder_create_call(IRBuilder *builder, IRValue *callee,
                                      IRValue **args, int num_args,
                                      const char *name);

// Terminator Creation
IRInstruction *ir_builder_create_br(IRBuilder *builder, IRBasicBlock *dest);
IRInstruction *ir_builder_create_cond_br(IRBuilder *builder, IRValue *cond,
                                         IRBasicBlock *true_dest,
                                         IRBasicBlock *false_dest);
IRInstruction *ir_builder_create_ret(IRBuilder *builder, IRValue *val);
IRInstruction *ir_builder_create_ret_void(IRBuilder *builder);

// Value & Block Creation
IRValue *ir_builder_create_reg(IRBuilder *builder, Type *type,
                               const char *name_prefix);
IRValue *ir_builder_create_const_int(IRBuilder *builder, int value);
IRValue *ir_builder_create_const_float(IRBuilder *builder, float value);
IRValue *ir_builder_create_const_double(IRBuilder *builder, double value);
IRBasicBlock *ir_builder_create_block(IRBuilder *builder,
                                      const char *name_prefix);

IRInstruction *ir_builder_create_sext(IRBuilder *builder, IRValue *src,
                                      Type *dest_type, const char *name);
IRInstruction *ir_builder_create_trunc(IRBuilder *builder, IRValue *src,
                                       Type *dest_type, const char *name);

#endif // IR_BUILDER_H