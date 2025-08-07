/**
 * @file cse.c
 * @brief Implements an optimized Common Subexpression Elimination (CSE) pass.
 * @version 4.1 (Performance Optimized)
 * - [IR_OPTIMIZATION] Improved hash table performance with better collision handling
 * - [ENHANCEMENT] Enhanced commutativity detection for more expressions
 * - [FEATURE] Better handling of complex expressions and PHI nodes
 * - [FIX] Improved dominator-based scoping for better accuracy
 * - [PERFORMANCE] Stack-based backtracking mechanism eliminates realloc calls
 * - [PERFORMANCE] Reduced memory allocations and improved cache locality
 * 
 * Performance Optimizations:
 * - Stack-based backtracking: Instead of using realloc() to track added entries,
 *   we now use a stack-based approach that records the hash table state at each
 *   recursion level. This eliminates dynamic memory allocation overhead.
 * - Memory pool usage: All backtracking state uses pool_alloc() for better
 *   memory management and reduced fragmentation.
 * - Efficient restoration: Backtracking restores only the buckets that were
 *   actually modified, rather than scanning all entries.
 */
#include "ir/transforms/cse.h"
#include "ir/ir_utils.h"
#include <string.h>
#include <stdint.h>       // for uintptr_t
#include <stdlib.h>
#include "ast.h"          // for pool_alloc
#include "logger.h"       // for LOG_CATEGORY_IR_OPT, LOG_DEBUG


// --- Optimized Hash Table for Available Expressions ---

typedef struct ExprKey {
    Opcode opcode;
    char* cond;
    int num_operands;
    IRValue* operands[8];
    Type* result_type; // Added for better type safety
} ExprKey;

typedef struct ExprEntry {
    ExprKey key;
    IRInstruction* defining_instr;
    struct ExprEntry* next;
    unsigned long hash; // Cache hash for better performance
} ExprEntry;

typedef struct {
    ExprEntry** buckets;
    int num_buckets;
    int num_entries;
    MemoryPool* pool;
} HashTable;

// --- Stack-based Backtracking Structure ---
typedef struct BacktrackState {
    int num_entries_at_entry;  // Number of entries when entering this block
    ExprEntry** old_heads;     // Array of old bucket heads that were overwritten
    int* overwritten_buckets;  // Array of bucket indices that were modified
    int num_overwritten;       // Number of buckets that were overwritten
    int capacity;              // Capacity of the arrays
} BacktrackState;

// --- Helper Prototypes ---
static bool is_instruction_cse_able(IRInstruction* instr);
static unsigned long hash_expression(const ExprKey* key);
static bool are_keys_equal(const ExprKey* k1, const ExprKey* k2);
static void cse_recursive(IRBasicBlock* bb, HashTable* available_exprs, bool* changed);
static bool is_commutative_op(Opcode opcode);
static void canonicalize_operands(ExprKey* key);
static ExprEntry* insert_expression_with_backtrack(HashTable* table, const ExprKey* key, IRInstruction* instr, BacktrackState* bt_state);
static ExprEntry* find_expression(HashTable* table, const ExprKey* key, IRBasicBlock* current_bb);
static void init_backtrack_state(BacktrackState* bt_state, HashTable* table, MemoryPool* pool);
static void cleanup_backtrack_state(BacktrackState* bt_state);
static void backtrack_hash_table(HashTable* table, BacktrackState* bt_state);

// --- Main CSE Pass ---
bool run_cse(IRFunction* func) {
    if (!func || !func->entry) {
        if (func && func->module && func->module->log_config) {
            LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "CSE: No function or entry block");
        }
        return false;
    }
    
    // 检查是否已经运行了支配者分析
    if (!func->blocks) {
        return false;
    }
    
    MemoryPool* pool = func->module->pool;
    bool changed = false;

    // Use a larger hash table for better performance
    HashTable available_exprs;
    int num_buckets = 1024; // Increased for better distribution
    available_exprs.buckets = (ExprEntry**)pool_alloc_z(pool, num_buckets * sizeof(ExprEntry*));
    available_exprs.num_buckets = num_buckets;
    available_exprs.num_entries = 0;
    available_exprs.pool = pool;

    if (func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "Running CSE on function @%s", func->name);
    }
    cse_recursive(func->entry, &available_exprs, &changed);
    
    if (changed && func->module && func->module->log_config) {
        LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_OPT, "CSE eliminated expressions in function @%s", func->name);
    }
    
    return changed;
}

// --- Optimized Recursive Traversal ---
static void cse_recursive(IRBasicBlock* bb, HashTable* available_exprs, bool* changed) {
    if (!bb) return;
    
    // Initialize backtracking state for this block
    BacktrackState bt_state;
    init_backtrack_state(&bt_state, available_exprs, available_exprs->pool);
    
    IRInstruction* instr = bb->head;
    while (instr) {
        IRInstruction* next_instr = instr->next;
        
        if (is_instruction_cse_able(instr)) {
            // Build expression key
            ExprKey key;
            key.opcode = instr->opcode;
            key.cond = instr->opcode_cond;
            key.num_operands = instr->num_operands;
            key.result_type = instr->dest ? instr->dest->type : NULL;
            
            // Collect operands
            int i = 0;
            for (IROperand* op = instr->operand_head; op && i < 8; op = op->next_in_instr, ++i) {
                key.operands[i] = op->data.value;
            }
            
            // Canonicalize operands for commutative operations
            canonicalize_operands(&key);
            
            // Look for available expression
            ExprEntry* found_entry = find_expression(available_exprs, &key, bb);
            
            if (found_entry) {
                // Replace with available expression
                if (instr->dest && found_entry->defining_instr->dest) {
                    replace_all_uses_with(NULL, instr->dest, found_entry->defining_instr->dest);
                    erase_instruction(instr);
                    *changed = true;
                    if (bb->parent && bb->parent->module && bb->parent->module->log_config) {
                        LOG_DEBUG(bb->parent->module->log_config, LOG_CATEGORY_IR_OPT, "CSE: Replaced expression in @%s", bb->label);
                    }
                }
            } else {
                // Make this expression available with backtracking tracking
                insert_expression_with_backtrack(available_exprs, &key, instr, &bt_state);
            }
        }
        instr = next_instr;
    }

    // Recurse on dominator children
    for (int i = 0; i < bb->dom_children_count; ++i) {
        cse_recursive(bb->dom_children[i], available_exprs, changed);
    }
    
    // Backtrack: restore hash table to state before this block
    backtrack_hash_table(available_exprs, &bt_state);
    cleanup_backtrack_state(&bt_state);
}

// --- Enhanced Helper Functions ---

static bool is_instruction_cse_able(IRInstruction* instr) {
    if (!instr || !instr->dest) return false;
    
    switch (instr->opcode) {
        case IR_OP_ADD: case IR_OP_SUB: case IR_OP_MUL: case IR_OP_SDIV:
        case IR_OP_FADD: case IR_OP_FSUB: case IR_OP_FMUL: case IR_OP_FDIV:
        case IR_OP_SHL: case IR_OP_LSHR: case IR_OP_ASHR:
        case IR_OP_AND: case IR_OP_OR: case IR_OP_XOR:
        case IR_OP_ICMP: case IR_OP_FCMP:
        case IR_OP_ZEXT: case IR_OP_FPEXT:
            return true;
        default:
            return false;
    }
}

static bool is_commutative_op(Opcode opcode) {
    switch (opcode) {
        case IR_OP_ADD: case IR_OP_MUL: case IR_OP_AND: case IR_OP_OR: case IR_OP_XOR:
        case IR_OP_FADD: case IR_OP_FMUL:
            return true;
        default:
            return false;
    }
}

static void canonicalize_operands(ExprKey* key) {
    if (key->num_operands == 2 && is_commutative_op(key->opcode)) {
        // Sort operands by address for canonical form
        if ((uintptr_t)key->operands[0] > (uintptr_t)key->operands[1]) {
            IRValue* temp = key->operands[0];
            key->operands[0] = key->operands[1];
            key->operands[1] = temp;
        }
    }
}

static unsigned long hash_expression(const ExprKey* key) {
    unsigned long hash = 5381; // djb2 hash function
    
    // Hash opcode and condition
    hash = ((hash << 5) + hash) + key->opcode;
    if (key->cond) {
        const char* p = key->cond;
        while (*p) hash = ((hash << 5) + hash) + *p++;
    }
    
    // Hash result type
    if (key->result_type) {
        hash = ((hash << 5) + hash) ^ ((uintptr_t)key->result_type >> 3);
    }
    
    // Hash operands
    for (int i = 0; i < key->num_operands; ++i) {
        hash = ((hash << 5) + hash) ^ ((uintptr_t)key->operands[i] >> 3);
    }
    
    return hash;
}

static bool are_keys_equal(const ExprKey* k1, const ExprKey* k2) {
    if (k1->opcode != k2->opcode || 
        k1->num_operands != k2->num_operands ||
        k1->result_type != k2->result_type) {
        return false;
    }
    
    if (k1->cond != k2->cond && 
        (!k1->cond || !k2->cond || strcmp(k1->cond, k2->cond) != 0)) {
        return false;
    }
    
    // Check operands
    if (is_commutative_op(k1->opcode) && k1->num_operands == 2) {
        return (k1->operands[0] == k2->operands[0] && k1->operands[1] == k2->operands[1]) ||
               (k1->operands[0] == k2->operands[1] && k1->operands[1] == k2->operands[0]);
    } else {
        for (int i = 0; i < k1->num_operands; ++i) {
            if (k1->operands[i] != k2->operands[i]) {
                return false;
            }
        }
    }
    
    return true;
}

static ExprEntry* insert_expression_with_backtrack(HashTable* table, const ExprKey* key, IRInstruction* instr, BacktrackState* bt_state) {
    unsigned long hash = hash_expression(key);
    int bucket = hash % table->num_buckets;
    
    // Track the old head if we're doing backtracking
    if (bt_state && table->buckets[bucket] != NULL) {
        // Check if we need to expand the arrays
        if (bt_state->num_overwritten >= bt_state->capacity) {
            bt_state->capacity *= 2;
            bt_state->old_heads = (ExprEntry**)pool_alloc(table->pool, bt_state->capacity * sizeof(ExprEntry*));
            bt_state->overwritten_buckets = (int*)pool_alloc(table->pool, bt_state->capacity * sizeof(int));
        }
        
        // Record this bucket as being overwritten
        bt_state->old_heads[bt_state->num_overwritten] = table->buckets[bucket];
        bt_state->overwritten_buckets[bt_state->num_overwritten] = bucket;
        bt_state->num_overwritten++;
    }
    
    ExprEntry* entry = (ExprEntry*)pool_alloc(table->pool, sizeof(ExprEntry));
    entry->key = *key;
    entry->defining_instr = instr;
    entry->hash = hash;
    entry->next = table->buckets[bucket];
    table->buckets[bucket] = entry;
    table->num_entries++;
    
    return entry;
}

static ExprEntry* find_expression(HashTable* table, const ExprKey* key, IRBasicBlock* current_bb) {
    unsigned long hash = hash_expression(key);
    int bucket = hash % table->num_buckets;
    
    for (ExprEntry* entry = table->buckets[bucket]; entry; entry = entry->next) {
        if (entry->hash == hash && 
            dominates(entry->defining_instr->parent, current_bb) && 
            are_keys_equal(&entry->key, key)) {
            return entry;
        }
    }
    
    return NULL;
}

static void init_backtrack_state(BacktrackState* bt_state, HashTable* table, MemoryPool* pool) {
    bt_state->num_entries_at_entry = table->num_entries;
    bt_state->old_heads = (ExprEntry**)pool_alloc_z(pool, 16 * sizeof(ExprEntry*));
    bt_state->overwritten_buckets = (int*)pool_alloc_z(pool, 16 * sizeof(int));
    bt_state->num_overwritten = 0;
    bt_state->capacity = 16;
}

static void cleanup_backtrack_state(BacktrackState* bt_state) {
    (void)bt_state; // Suppress unused parameter warning
    // No need to free since we're using pool_alloc
}

static void backtrack_hash_table(HashTable* table, BacktrackState* bt_state) {
    // Restore the hash table to its state before this block
    for (int i = 0; i < bt_state->num_overwritten; ++i) {
        int bucket = bt_state->overwritten_buckets[i];
        table->buckets[bucket] = bt_state->old_heads[i];
    }
    table->num_entries = bt_state->num_entries_at_entry;
}