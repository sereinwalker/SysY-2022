#ifndef IR_UTILS_H
#define IR_UTILS_H

#include "ir/ir_data_structures.h"
#include <stdint.h> // For uint64_t
#include <stddef.h>
#include <stdbool.h>
#include "ast.h"                    // for SymbolTable

// 前向声明
typedef struct SymbolTable SymbolTable;
typedef struct IRBuilder IRBuilder;

// --- 基础结构体 ---
typedef struct Worklist {
    void** items;
    int capacity;
    int count;
    MemoryPool* pool;
} Worklist;

typedef struct BitSet {
    uint64_t* words;
    int num_words;
} BitSet;

typedef struct ValueMapEntry {
    IRValue* old_val;
    IRValue* new_val;
    struct ValueMapEntry* next_in_hash;
} ValueMapEntry;

typedef struct ValueMap {
    ValueMapEntry* entries;
    int count;
    int capacity;
    MemoryPool* pool;
    ValueMapEntry** hash_table;
    int hash_capacity;
} ValueMap;

// --- IR对象创建相关函数 ---
IRValue* create_ir_value(MemoryPool* pool);
IRInstruction* create_ir_instruction(Opcode opcode, MemoryPool* pool);
IRBasicBlock* create_ir_basic_block(const char* label, IRFunction* func, MemoryPool* pool);
IRFunction* create_ir_function(const char* name, Type* return_type, IRModule* module, MemoryPool* pool);
IRGlobalVariable* create_ir_global_variable(const char* name, Type* type, bool is_const, MemoryPool* pool);
IROperand* create_ir_operand(OperandKind kind, void* data, IRInstruction* user, MemoryPool* pool);
IRValue* create_constant_i1(bool val, MemoryPool* pool);
IRValue* create_constant_i64(int64_t val, MemoryPool* pool);
IRValue* create_constant_double(double val, MemoryPool* pool);

// --- IR对象链接函数 ---
void link_function_to_module(IRFunction* func, IRModule* module);
void link_basic_block_to_function(IRBasicBlock* bb, IRFunction* func);
void link_instruction_to_basic_block(IRInstruction* instr, IRBasicBlock* bb);
void link_global_to_module(IRGlobalVariable* global, IRModule* module);

// --- IR对象查询函数 ---
bool is_binary_operation(IRInstruction* instr);
bool is_unary_operation(IRInstruction* instr);
bool is_memory_operation(IRInstruction* instr);
bool has_side_effects(IRInstruction* instr);
bool is_constant(IRValue* val);
bool is_register(IRValue* val);
bool is_global(IRValue* val);
bool is_type_same(Type* t1, Type* t2, bool strict);
bool is_pure_function_call(IRInstruction* instr);

// --- IR对象验证函数 ---
bool validate_instruction(IRInstruction* instr);
bool validate_basic_block(IRBasicBlock* bb);
bool validate_function(IRFunction* func);
bool validate_module(IRModule* module);

// --- IR修改工具 ---
void replace_all_uses_with(Worklist* wl, IRValue* old_val, IRValue* new_val);
void erase_instruction(IRInstruction* instr);
void change_operand_value(IROperand* op, IRValue* new_val);
void remove_operand(IROperand* op);
void insert_instr_after(IRInstruction* new_instr, IRInstruction* pos);
void insert_instr_before(IRInstruction* new_instr, IRInstruction* pos);
void add_instr_to_bb_end(IRBasicBlock* bb, IRInstruction* instr);
void add_value_operand(IRInstruction* instr, IRValue* val);
void add_bb_operand(IRInstruction* instr, IRBasicBlock* bb);
IROperand* add_operand(IRInstruction* instr, OperandKind kind, void* data_ptr);

// --- CFG/支配树/PHI/块操作相关工具 ---
void remove_predecessor(IRBasicBlock* block, IRBasicBlock* pred_to_remove);
void remove_successor(IRBasicBlock* block, IRBasicBlock* succ_to_remove);
void add_successor(IRBasicBlock* block, IRBasicBlock* new_succ);
void add_predecessor(IRBasicBlock* block, IRBasicBlock* new_pred);
void change_terminator_target(IRInstruction* term, IRBasicBlock* from, IRBasicBlock* to);
void change_phi_predecessor(IRBasicBlock* block, IRBasicBlock* from, IRBasicBlock* to);
bool dominates(IRBasicBlock* dom, IRBasicBlock* use);
void compute_dom_tree_timestamps(IRFunction* func);
bool is_terminator_instruction(IRInstruction* instr);
IRValue* phi_get_incoming_value_for_block(IRInstruction* phi, IRBasicBlock* block);
void remove_phi_entries_for_predecessor(IRInstruction* phi, IRBasicBlock* pred);
void insert_block_after(IRBasicBlock* new_bb, IRBasicBlock* pos);
void move_instructions_to_block_end(IRBasicBlock* from, IRBasicBlock* to);
void replace_all_uses_with_block(IRBasicBlock* from, IRBasicBlock* to);
void remove_block_from_function(IRBasicBlock* bb);
void redirect_edge(IRBasicBlock* from, IRBasicBlock* old_to, IRBasicBlock* new_to);
void sever_all_successors(IRBasicBlock* bb);
void repair_phi_nodes_after_edge_redirect(IRBasicBlock* new_to, IRBasicBlock* from, IRBasicBlock* old_to);
IRValue* get_undef_value(Type* type, MemoryPool* pool);

// --- Worklist/BitSet管理 ---
Worklist* create_worklist(MemoryPool* pool, int initial_capacity);
void worklist_add(Worklist* wl, void* item);
void* worklist_pop(Worklist* wl);
bool worklist_empty(Worklist* wl);
void destroy_worklist(Worklist* wl);
BitSet* bitset_create(int num_elements, MemoryPool* pool);
void bitset_add(BitSet* bs, int id, LogConfig* log_config);
bool bitset_contains(struct BitSet* bs, int id);
void bitset_copy(BitSet* dest, const BitSet* src);
bool bitset_equals(const BitSet* bs1, const BitSet* bs2);
void bitset_intersect(BitSet* dest, const BitSet* src);
void bitset_set_all(BitSet* bs, int num_elements);
void bitset_clear(BitSet* bs);
void bitset_union(BitSet* dest, const BitSet* src);
void destroy_bitset(BitSet* bs);

// --- IR遍历工具 ---
typedef void (*InstructionVisitor)(IRInstruction* instr, void* user_data);
typedef void (*BasicBlockVisitor)(IRBasicBlock* bb, void* user_data);
typedef void (*FunctionVisitor)(IRFunction* func, void* user_data);
void visit_instructions(IRBasicBlock* bb, InstructionVisitor visitor, void* user_data);
void visit_basic_blocks(IRFunction* func, BasicBlockVisitor visitor, void* user_data);
void visit_functions(IRModule* module, FunctionVisitor visitor, void* user_data);

// --- IR克隆/重映射/循环分析工具 ---
IRInstruction* clone_instruction(IRInstruction* instr, MemoryPool* pool);
IRInstruction* clone_instruction_with_remap(
    IRInstruction* instr, 
    IRBuilder* builder, 
    ValueMap* remap
);
void remap_instruction_operands(IRInstruction* instr, ValueMap* value_remap);
void value_map_merge(ValueMap* dst, ValueMap* src);
IRValue* remap_value(ValueMap* map, IRValue* old_val);
Worklist* get_loops_sorted_by_depth(IRFunction* func);
void recalculate_instruction_count(IRFunction* func);

// --- ValueMap管理工具 ---
void value_map_init(ValueMap* map, MemoryPool* pool);
void value_map_put(ValueMap* map, IRValue* old_val, IRValue* new_val, LogConfig* log_config);
IRValue* value_map_get(const ValueMap* map, IRValue* old_val, LogConfig* log_config);

// --- pool_alloc_z ---
void* pool_alloc_z(MemoryPool* pool, size_t size);

#endif // IR_UTILS_H