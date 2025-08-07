#include "ir/ir_utils.h"
#include "ast.h"
#include "ir/ir_builder.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/******************************************************************************
 *                                                                            *
 *                         内部辅助函数 - 前置声明                             *
 *                                                                            *
 ******************************************************************************/

/**
 * @brief (内部函数) 从一个值的 Use 链表中移除一个操作数。
 * @details 这是维护 Use-Def/Def-Use 链的核心底层操作。
 *          它会找到 op 所引用的 IRValue，并遍历其 use_list，将 op 从中移除。
 * @param op 要移除的操作数。
 */
static void remove_operand_from_use_list(IROperand *op);

/**
 * @brief (内部函数) 计算一个 IRValue 的哈希值。
 * @details 主要基于值的指针地址，如果是常量，则额外考虑其内容。
 * @param val 要计算哈希的 IRValue。
 * @return 计算出的哈希值。
 */
static unsigned int hash_value(IRValue *val);

/**
 * @brief (内部函数) 对支配树进行深度优先搜索，以计算时间戳。
 * @param bb 当前基本块。
 * @param time 指向当前时间戳的指针。
 */
static void dfs_dom_tree(IRBasicBlock *bb, int *time);

/**
 * @brief (内部函数) 移除PHI指令中的一对操作数（值和块）。
 * @param op_val 指向值操作数的指针，将和其后继的块操作数一起被移除。
 */
static void remove_phi_operand_pair(IROperand *op_val);

/******************************************************************************
 *                                                                            *
 *                      1. 基础数据结构实现                                  *
 *                (Worklist, ValueMap, BitSet)                               *
 *                                                                            *
 ******************************************************************************/

// --- 通用工具 ---

/**
 * @brief 从内存池分配一块内存并将其所有字节清零。
 * @param pool 用于分配的内存池。
 * @param size 要分配的字节数。
 * @return 指向已分配并清零的内存的指针，若分配失败则为 NULL。
 */
void *pool_alloc_z(MemoryPool *pool, size_t size) {
  void *mem = pool_alloc(pool, size);
  if (mem) {
    memset(mem, 0, size);
  }
  return mem;
}

// --- Worklist 实现 (作为栈使用) ---

/**
 * @brief 创建一个工作列表。
 * @param pool 用于分配的内存池。
 * @param initial_capacity 列表的初始容量。
 * @return 指向新创建的 Worklist 的指针。
 */
Worklist *create_worklist(MemoryPool *pool, int initial_capacity) {
  Worklist *wl = (Worklist *)pool_alloc(pool, sizeof(Worklist));
  if (initial_capacity <= 0) {
    initial_capacity = 16;
  }
  wl->items = (void **)pool_alloc(pool, initial_capacity * sizeof(void *));
  wl->capacity = initial_capacity;
  wl->count = 0;
  wl->pool = pool;
  return wl;
}

/**
 * @brief 向工作列表（栈顶）添加一个项。
 * @details
 * 如果容量不足，会自动以2倍因子扩容。为了避免频繁扩容，使用更保守的增长策略。
 * @param wl 目标工作列表。
 * @param item 要添加的项。
 */
void worklist_add(Worklist *wl, void *item) {
  if (!item)
    return;

  // 检查是否需要扩容
  if (wl->count >= wl->capacity) {
    // 使用更保守的增长策略：小容量时翻倍，大容量时增长50%
    int new_capacity;
    if (wl->capacity < 1024) {
      new_capacity = wl->capacity * 2;
    } else {
      new_capacity = wl->capacity + (wl->capacity >> 1); // 增长50%
    }

    void **new_items =
        (void **)pool_alloc(wl->pool, new_capacity * sizeof(void *));
    if (!new_items) {
      // 内存分配失败，尝试最小扩容
      new_capacity = wl->capacity + 1;
      new_items = (void **)pool_alloc(wl->pool, new_capacity * sizeof(void *));
      if (!new_items)
        return; // 彻底失败，放弃添加
    }

    // 使用更高效的内存复制
    if (wl->count > 0) {
      memcpy(new_items, wl->items, wl->count * sizeof(void *));
    }
    wl->items = new_items;
    wl->capacity = new_capacity;
  }
  wl->items[wl->count++] = item;
}

/**
 * @brief 从工作列表（栈顶）弹出一个项。
 * @param wl 目标工作列表。
 * @return 弹出的项，如果列表为空则为 NULL。
 */
void *worklist_pop(Worklist *wl) {
  if (wl->count == 0) {
    return NULL;
  }
  return wl->items[--wl->count];
}

// --- ValueMap 实现 (哈希映射表) ---

/**
 * @brief 初始化一个值映射表。
 * @param map 指向要初始化的 ValueMap 的指针。
 * @param pool 用于分配的内存池。
 */
void value_map_init(ValueMap *map, MemoryPool *pool) {
  map->pool = pool;
  map->capacity = 32;
  map->count = 0;
  map->entries =
      (ValueMapEntry *)pool_alloc(pool, sizeof(ValueMapEntry) * map->capacity);

  // 初始化哈希桶
  map->hash_capacity = map->capacity;
  map->hash_table = (ValueMapEntry **)pool_alloc_z(
      pool, sizeof(ValueMapEntry *) * map->hash_capacity);
}

/**
 * @brief (内部函数) 计算一个 IRValue 的哈希值。
 */
static unsigned int hash_value(IRValue *val) {
  if (!val)
    return 0;

  // 使用黄金比例常数进行乘法哈希，提高哈希质量
  const unsigned int HASH_MULTIPLIER = 2654435761U; // (sqrt(5) - 1) / 2 * 2^32

  // 如果是常量，优先基于其值计算哈希
  if (val->is_constant && val->type->kind == TYPE_BASIC) {
    unsigned int content_hash = 0;
    switch (val->type->basic) {
    case BASIC_INT:
      content_hash = (unsigned int)val->int_val;
      break;
    case BASIC_FLOAT: {
      // 使用 union 更安全地进行位级转换
      union {
        float f;
        uint32_t i;
      } converter = {.f = val->float_val};
      content_hash = converter.i;
      break;
    }
    case BASIC_DOUBLE: {
      // 对于64位双精度浮点数，将高低32位异或
      union {
        double d;
        uint64_t i;
      } converter = {.d = val->double_val};
      uint64_t bits = converter.i;
      content_hash = (unsigned int)(bits ^ (bits >> 32));
      break;
    }
    case BASIC_I1:
      // i1类型通常存储在int_val中，0表示false，非0表示true
      content_hash = val->int_val ? 1 : 0;
      break;
    case BASIC_I8:
      // i8类型也存储在int_val中
      content_hash = (unsigned int)(val->int_val & 0xFF);
      break;
    default:
      // 对于未知类型，使用指针地址
      content_hash = (unsigned int)((uintptr_t)val >> 3);
      break;
    }
    return content_hash * HASH_MULTIPLIER;
  }

  // 对于非常量值，基于指针地址计算哈希
  // 处理64位指针的截断问题
  uintptr_t ptr = (uintptr_t)val;
  unsigned int addr_hash;

  if (sizeof(uintptr_t) == 8) {
    // 64位系统：将高低32位异或后再右移
    addr_hash = (unsigned int)((ptr ^ (ptr >> 32)) >> 3);
  } else {
    // 32位系统：直接右移
    addr_hash = (unsigned int)(ptr >> 3);
  }

  return addr_hash * HASH_MULTIPLIER;
}

/**
 * @brief 在值映射表中添加或更新一个 "旧值 -> 新值" 的映射。
 * @param map 目标值映射表。
 * @param old_val 作为键的旧值。
 * @param new_val 作为值的新值。
 */
void value_map_put(ValueMap *map, IRValue *old_val, IRValue *new_val,
                   LogConfig *log_config) {
  // 常量不应作为键被重映射
  if (!old_val || old_val->is_constant)
    return;

  // 检查是否需要扩容，如果需要则对整个表进行 rehash
  if (map->count >= map->capacity) {
    if (log_config) {
      LOG_DEBUG(log_config, LOG_CATEGORY_MEMORY,
                "ValueMap expanding from capacity %d", map->capacity);
    }
    int old_cap = map->capacity;
    int new_capacity = map->capacity * 2;

    // 尝试分配新的条目数组
    ValueMapEntry *new_entries = (ValueMapEntry *)pool_alloc(
        map->pool, sizeof(ValueMapEntry) * new_capacity);
    if (!new_entries) {
      // 内存分配失败，尝试最小扩容
      new_capacity = map->capacity + 1;
      new_entries = (ValueMapEntry *)pool_alloc(
          map->pool, sizeof(ValueMapEntry) * new_capacity);
      if (!new_entries)
        return; // 彻底失败，放弃添加
    }

    // 复制现有条目
    if (map->count > 0) {
      memcpy(new_entries, map->entries, sizeof(ValueMapEntry) * old_cap);
    }
    map->entries = new_entries;
    map->capacity = new_capacity;

    // 重新计算哈希表大小，使用更好的负载因子
    int new_hash_capacity = map->capacity;
    // 确保哈希表大小是2的幂，提高性能
    while (new_hash_capacity & (new_hash_capacity - 1)) {
      new_hash_capacity++;
    }

    ValueMapEntry **new_hash_table = (ValueMapEntry **)pool_alloc_z(
        map->pool, sizeof(ValueMapEntry *) * new_hash_capacity);
    if (!new_hash_table) {
      // 哈希表分配失败，使用线性搜索
      map->hash_capacity = 0;
      map->hash_table = NULL;
      return;
    }

    map->hash_capacity = new_hash_capacity;
    map->hash_table = new_hash_table;

    // 重新构建哈希表
    for (int i = 0; i < map->count; ++i) {
      unsigned int hash =
          hash_value(map->entries[i].old_val) % map->hash_capacity;
      map->entries[i].next_in_hash = map->hash_table[hash];
      map->hash_table[hash] = &map->entries[i];
    }
  }

  // 检查键是否已存在，若存在则更新
  unsigned int hash = hash_value(old_val) % map->hash_capacity;
  for (ValueMapEntry *entry = map->hash_table[hash]; entry;
       entry = entry->next_in_hash) {
    if (entry->old_val == old_val) {
      entry->new_val = new_val;
      return;
    }
  }

  // 添加新条目到线性数组，并插入到哈希桶链表的头部
  ValueMapEntry *entry = &map->entries[map->count];
  entry->old_val = old_val;
  entry->new_val = new_val;
  entry->next_in_hash = map->hash_table[hash];
  map->hash_table[hash] = entry;
  map->count++;
}

/**
 * @brief 在值映射表中查找一个旧值，返回对应的新值。
 * @param map 目标值映射表。
 * @param old_val 要查找的键。
 * @return 找到则返回对应的新值，否则返回 NULL。
 */
IRValue *value_map_get(const ValueMap *map, IRValue *old_val,
                       LogConfig *log_config) {
  if (!old_val || !map) {
    if (log_config) {
      LOG_DEBUG(log_config, LOG_CATEGORY_MEMORY,
                "ValueMap lookup failed: invalid parameters");
    }
    return NULL;
  }

  unsigned int hash = hash_value(old_val) % map->hash_capacity;
  for (ValueMapEntry *entry = map->hash_table[hash]; entry;
       entry = entry->next_in_hash) {
    if (entry->old_val == old_val) {
      if (log_config) {
        LOG_DEBUG(log_config, LOG_CATEGORY_MEMORY,
                  "ValueMap lookup successful for value %s",
                  old_val->name ? old_val->name : "unnamed");
      }
      return entry->new_val;
    }
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_MEMORY,
              "ValueMap lookup failed: value not found");
  }
  return NULL;
}

/**
 * @brief 重新映射一个值。
 * @details
 * 这是一个便捷函数，如果在映射表中找到旧值，则返回新值，否则返回旧值本身。
 * @param map 目标值映射表。
 * @param old_val 要重新映射的值。
 * @return 最终的值。
 */
IRValue *remap_value(ValueMap *map, IRValue *old_val) {
  IRValue *new_val = value_map_get(map, old_val, NULL);
  return new_val ? new_val : old_val;
}

// --- BitSet 实现 ---

/**
 * @brief 创建一个位集合。
 * @param num_elements 集合能容纳的最大元素ID+1。
 * @param pool 用于分配的内存池。
 * @return 指向新创建的 BitSet 的指针。
 */
BitSet *bitset_create(int num_elements, MemoryPool *pool) {
  BitSet *bs = (BitSet *)pool_alloc(pool, sizeof(BitSet));
  int num_words = (num_elements + 63) / 64; // 向上取整计算需要的uint64_t数量
  bs->num_words = num_words;
  bs->words = (uint64_t *)pool_alloc_z(pool, sizeof(uint64_t) * num_words);
  return bs;
}

/**
 * @brief 向位集合中添加一个元素。
 * @param bs 目标位集合。
 * @param id 要添加的元素的ID。
 */
void bitset_add(BitSet *bs, int id, LogConfig *log_config) {
  if (!bs || id < 0)
    return; // 防御性检查

  int word_index = id / 64;
  int bit_index = id % 64;

  // 边界检查，避免数组越界
  if (word_index >= bs->num_words) {
    if (log_config) {
      LOG_WARN(log_config, LOG_CATEGORY_MEMORY,
               "BitSet access out of bounds: id %d is too large.", id);
    }
    return;
  }

  bs->words[word_index] |= (1ULL << bit_index);
}

/**
 * @brief 检查位集合中是否包含一个元素。
 * @param bs 目标位集合。
 * @param id 要检查的元素的ID。
 * @return 如果包含则为 true，否则为 false。
 */
bool bitset_contains(BitSet *bs, int id) {
  if (!bs || id < 0)
    return false; // 防御性检查

  int word_index = id / 64;
  int bit_index = id % 64;

  // 边界检查，避免数组越界
  if (word_index >= bs->num_words) {
    return false; // ID超出范围，返回false
  }

  return (bs->words[word_index] & (1ULL << bit_index)) != 0;
}

/**
 * @brief 将一个位集合的内容复制到另一个。
 * @param dest 目标位集合。
 * @param src 源位集合。
 */
void bitset_copy(BitSet *dest, const BitSet *src) {
  if (!dest || !src || !dest->words || !src->words)
    return; // 防御性检查

  // 确保两个位集合大小相同
  if (dest->num_words != src->num_words) {
    // 大小不匹配，只复制较小的部分
    int min_words =
        dest->num_words < src->num_words ? dest->num_words : src->num_words;
    if (min_words > 0) {
      memcpy(dest->words, src->words, min_words * sizeof(uint64_t));
    }
    return;
  }

  if (dest->num_words > 0) {
    memcpy(dest->words, src->words, dest->num_words * sizeof(uint64_t));
  }
}

/**
 * @brief 计算两个位集合的交集，结果存入第一个集合。
 * @param dest 目标位集合，也将存储结果。
 * @param src 源位集合。
 */
void bitset_intersect(BitSet *dest, const BitSet *src) {
  assert(dest->num_words == src->num_words);
  for (int i = 0; i < dest->num_words; ++i) {
    dest->words[i] &= src->words[i];
  }
}

/**
 * @brief 检查两个位集合是否完全相等。
 * @param bs1 第一个位集合。
 * @param bs2 第二个位集合。
 * @return 如果相等则为 true，否则为 false。
 */
bool bitset_equals(const BitSet *bs1, const BitSet *bs2) {
  assert(bs1->num_words == bs2->num_words);
  return memcmp(bs1->words, bs2->words, bs1->num_words * sizeof(uint64_t)) == 0;
}

/**
 * @brief 将位集合的所有位（最多到 num_elements）都设置为1。
 * @param bs 目标位集合。
 * @param num_elements 实际的元素数量。
 */
void bitset_set_all(BitSet *bs, int num_elements) {
  int num_words = (num_elements + 63) / 64;
  assert(num_words <= bs->num_words);
  memset(bs->words, 0xFF, num_words * sizeof(uint64_t));

  // 清除最后一个 word 中超出 num_elements 的多余位
  int remaining_bits = num_elements % 64;
  if (remaining_bits > 0 && num_words > 0) {
    // 创建一个低 remaining_bits 位全为1的掩码
    uint64_t mask = (1ULL << remaining_bits) - 1;
    bs->words[num_words - 1] &= mask;
  }
}

/******************************************************************************
 *                                                                            *
 *                      2. IR对象创建函数                                     *
 *                                                                            *
 ******************************************************************************/

/**
 * @brief 创建一个通用的、已清零的 IRValue。
 */
IRValue *create_ir_value(MemoryPool *pool) {
  return (IRValue *)pool_alloc_z(pool, sizeof(IRValue));
}

/**
 * @brief 创建一个带有指定操作码的 IRInstruction。
 */
IRInstruction *create_ir_instruction(Opcode opcode, MemoryPool *pool) {
  IRInstruction *instr =
      (IRInstruction *)pool_alloc_z(pool, sizeof(IRInstruction));
  instr->opcode = opcode;
  // operand_head 和 operand_tail 已被 pool_alloc_z 初始化为 NULL
  return instr;
}

/**
 * @brief 创建一个基本块，并设置其父函数。
 */
IRBasicBlock *create_ir_basic_block(const char *label, IRFunction *func,
                                    MemoryPool *pool) {
  IRBasicBlock *bb = (IRBasicBlock *)pool_alloc_z(pool, sizeof(IRBasicBlock));
  bb->label = pool_strdup(pool, label);
  bb->parent = func;
  return bb;
}

/**
 * @brief 创建一个函数，并设置其父模块。
 */
IRFunction *create_ir_function(const char *name, Type *return_type,
                               IRModule *module, MemoryPool *pool) {
  IRFunction *func = (IRFunction *)pool_alloc_z(pool, sizeof(IRFunction));
  func->name = pool_strdup(pool, name);
  func->return_type = return_type;
  func->module = module;
  return func;
}

/**
 * @brief 创建一个全局变量。
 */
IRGlobalVariable *create_ir_global_variable(const char *name, Type *type,
                                            bool is_const, MemoryPool *pool) {
  IRGlobalVariable *global =
      (IRGlobalVariable *)pool_alloc_z(pool, sizeof(IRGlobalVariable));
  global->name = pool_strdup(pool, name);
  global->type = type;
  global->is_const = is_const;
  return global;
}

/**
 * @brief 创建一个操作数，并设置其种类、数据和使用者。
 */
IROperand *create_ir_operand(OperandKind kind, void *data, IRInstruction *user,
                             MemoryPool *pool) {
  IROperand *operand = (IROperand *)pool_alloc_z(pool, sizeof(IROperand));
  operand->kind = kind;
  operand->user = user;
  if (kind == IR_OP_KIND_VALUE) {
    operand->data.value = (IRValue *)data;
  } else {
    operand->data.bb = (IRBasicBlock *)data;
  }
  return operand;
}

/**
 * @brief 创建一个 i1 类型的常量值。
 */
IRValue *create_constant_i1(bool val, MemoryPool *pool) {
  IRValue *v = create_ir_value(pool);
  v->is_constant = true;
  v->type = create_basic_type(BASIC_I1, true, pool); // 常量类型本身也是const
  v->int_val = val ? 1 : 0;
  return v;
}

/**
 * @brief 创建一个 i64 类型的常量值。
 */
IRValue *create_constant_i64(int64_t val, MemoryPool *pool) {
  IRValue *v = create_ir_value(pool);
  v->is_constant = true;
  v->type = create_basic_type(BASIC_I64, true, pool);
  v->i64_val = val;
  return v;
}

/**
 * @brief 创建一个 double 类型的常量值。
 */
IRValue *create_constant_double(double val, MemoryPool *pool) {
  IRValue *v = create_ir_value(pool);
  v->is_constant = true;
  v->type = create_basic_type(BASIC_DOUBLE, true, pool);
  v->double_val = val;
  return v;
}

/******************************************************************************
 *                                                                            *
 *              3. IR 修改与 Use-Def/Def-Use 链维护函数                       *
 *                                                                            *
 ******************************************************************************/

/**
 * @brief 向指令添加一个操作数，并正确维护所有相关链表。
 * @details 这是一个核心的 IR 修改函数。它负责：
 *          1. 创建 IROperand 对象。
 *          2. 将操作数添加到指令的操作数双向链表的末尾（O(1) 效率）。
 *          3. 如果操作数是 IRValue，将其添加到该值的 Use 链的头部。
 * @param instr 目标指令。
 * @param kind 操作数的种类。
 * @param data_ptr 指向操作数数据的指针 (IRValue* 或 IRBasicBlock*)。
 * @return 新创建的 IROperand。
 */
IROperand *add_operand(IRInstruction *instr, OperandKind kind, void *data_ptr) {
  if (!data_ptr)
    return NULL;

  // 从指令的父级结构中获取内存池
  MemoryPool *pool = instr->parent->parent->module->pool;
  IROperand *op = create_ir_operand(kind, data_ptr, instr, pool);

  // 将操作数添加到指令的操作数链表尾部
  if (!instr->operand_head) {
    instr->operand_head = op;
    instr->operand_tail = op;
  } else {
    instr->operand_tail->next_in_instr = op;
    op->prev_in_instr = instr->operand_tail;
    instr->operand_tail = op;
  }
  instr->num_operands++;

  // 如果是值操作数，更新其 Use 链
  if (kind == IR_OP_KIND_VALUE) {
    IRValue *val = op->data.value;
    if (!val->is_constant) {
      op->next_use = val->use_list_head;
      val->use_list_head = op;
    }
  }
  return op;
}

/**
 * @brief (类型安全封装) 向指令添加一个值类型的操作数。
 */
void add_value_operand(IRInstruction *instr, IRValue *val) {
  add_operand(instr, IR_OP_KIND_VALUE, val);
}

/**
 * @brief (类型安全封装) 向指令添加一个基本块类型的操作数。
 */
void add_bb_operand(IRInstruction *instr, IRBasicBlock *bb) {
  add_operand(instr, IR_OP_KIND_BASIC_BLOCK, bb);
}

/**
 * @brief (内部函数) 从一个值的 Use 链表中移除一个操作数。
 */
static void remove_operand_from_use_list(IROperand *op) {
  if (op->kind != IR_OP_KIND_VALUE)
    return;

  IRValue *val = op->data.value;
  if (!val || val->is_constant || val->def_instr == NULL)
    return;

  // 使用指向指针的指针技巧，优雅地处理头节点和中间节点的删除
  IROperand **p_prev_next_use = &val->use_list_head;
  while (*p_prev_next_use) {
    if (*p_prev_next_use == op) {
      *p_prev_next_use =
          op->next_use;    // 将前一个节点的next指向op的next，从而跳过op
      op->next_use = NULL; // 断开op的链接
      return;
    }
    p_prev_next_use = &(*p_prev_next_use)->next_use;
  }
}

/**
 * @brief 从指令中移除一个操作数，并正确维护所有相关链表。
 * @param op 要移除的操作数。
 */
void remove_operand(IROperand *op) {
  if (!op)
    return;

  IRInstruction *instr = op->user;

  // 1. 从其引用值的 Use 链中断开
  remove_operand_from_use_list(op);

  // 2. 从指令的操作数双向链表中解开
  if (op->prev_in_instr) {
    op->prev_in_instr->next_in_instr = op->next_in_instr;
  } else {
    instr->operand_head = op->next_in_instr;
  }

  if (op->next_in_instr) {
    op->next_in_instr->prev_in_instr = op->prev_in_instr;
  } else {
    instr->operand_tail = op->prev_in_instr;
  }

  instr->num_operands--;
}

/**
 * @brief 改变一个操作数引用的值，并正确更新 Use-Def 链。
 * @param op 要修改的操作数。
 * @param new_val 新引用的值。
 */
void change_operand_value(IROperand *op, IRValue *new_val) {
  assert(op->kind == IR_OP_KIND_VALUE &&
         "Can only change the value of a VALUE operand.");
  if (op->data.value == new_val)
    return;

  // 1. 从旧值的 Use 链中移除
  remove_operand_from_use_list(op);

  // 2. 更新操作数指向新值
  op->data.value = new_val;

  // 3. 将操作数添加到新值的 Use 链头部
  if (new_val && !new_val->is_constant && new_val->def_instr != NULL) {
    op->next_use = new_val->use_list_head;
    new_val->use_list_head = op;
  } else {
    op->next_use = NULL;
  }
}

/**
 * @brief 将一个值的所有使用者（use）替换为另一个新值。
 * @details 这是一个非常重要的优化工具函数。
 * @param wl (可选) 工作列表，用于将被修改指令的用户添加到其中，以待后续处理。
 * @param old_val 被替换的值。
 * @param new_val 新的值。
 */
void replace_all_uses_with(Worklist *wl, IRValue *old_val, IRValue *new_val) {
  if (old_val == new_val || !old_val->use_list_head)
    return;

  // 记录Use-Def链替换操作
  // 尝试从 old_val 的定义指令获取 LogConfig
  LogConfig *log_config = NULL;
  if (old_val && old_val->def_instr && old_val->def_instr->parent &&
      old_val->def_instr->parent->parent &&
      old_val->def_instr->parent->parent->module) {
    log_config = old_val->def_instr->parent->parent->module->log_config;
  }

  if (log_config) {
    LOG_TRACE(log_config, LOG_CATEGORY_IR_OPT,
              "Replacing all uses of value %s with %s",
              old_val->name ? old_val->name : "unnamed",
              new_val->name ? new_val->name : "unnamed");
  }

  // 循环处理 use_list_head，直到链表为空。
  // 每次循环，change_operand_value 都会将当前 head 从链表中移除。
  while (old_val->use_list_head) {
    IROperand *use = old_val->use_list_head;
    IRInstruction *user_instr = use->user;

    change_operand_value(use, new_val);

    // 如果提供了工作列表，将被修改的指令添加到其中，以便进行迭代优化。
    if (wl && user_instr && user_instr->opcode != IR_OP_UNKNOWN) {
      if (!user_instr->in_worklist) {
        worklist_add(wl, user_instr);
        user_instr->in_worklist = true;
      }
    }
  }
}

/**
 * @brief 在指定指令之后插入一条新指令。
 */
void insert_instr_after(IRInstruction *new_instr, IRInstruction *pos) {
  assert(pos && pos->parent &&
         "Position instruction must be valid and in a basic block.");
  assert(new_instr && !new_instr->parent &&
         "New instruction must not already be in a block.");

  IRBasicBlock *bb = pos->parent;
  new_instr->parent = bb;
  new_instr->prev = pos;
  new_instr->next = pos->next;

  if (pos->next) {
    pos->next->prev = new_instr;
  } else {
    bb->tail = new_instr; // pos 是原尾部
  }
  pos->next = new_instr;
}

/**
 * @brief 在指定指令之前插入一条新指令。
 */
void insert_instr_before(IRInstruction *new_instr, IRInstruction *pos) {
  assert(pos && pos->parent &&
         "Position instruction must be valid and in a basic block.");
  assert(new_instr && !new_instr->parent &&
         "New instruction must not already be in a block.");

  IRBasicBlock *bb = pos->parent;
  new_instr->parent = bb;
  new_instr->next = pos;
  new_instr->prev = pos->prev;

  if (pos->prev) {
    pos->prev->next = new_instr;
  } else {
    bb->head = new_instr; // pos 是原头部
  }
  pos->prev = new_instr;
}

/**
 * @brief 将一条指令添加到基本块的末尾（但在终结符之前）。
 */
void add_instr_to_bb_end(IRBasicBlock *bb, IRInstruction *instr) {
  assert(bb && instr);

  if (bb->tail && is_terminator_instruction(bb->tail)) {
    // 如果块已包含终结符，则插入到终结符之前
    insert_instr_before(instr, bb->tail);
  } else {
    // 否则，简单地追加到块的末尾
    instr->parent = bb;
    instr->prev = bb->tail;
    instr->next = NULL;
    if (bb->tail) {
      bb->tail->next = instr;
    } else {
      // 块是空的
      bb->head = instr;
    }
    bb->tail = instr;
  }

  if (bb->parent) {
    bb->parent->instruction_count++;
  }
}

/**
 * @brief 从IR中安全地删除一条指令。
 * @details 负责断开所有Use-Def链接，然后从基本块的指令链表中移除。
 * @param instr 要删除的指令。
 */
void erase_instruction(IRInstruction *instr) {
  if (!instr || instr->opcode == IR_OP_UNKNOWN)
    return;

  // 记录指令删除操作
  LogConfig *log_config = NULL;
  if (instr->parent && instr->parent->parent && instr->parent->parent->module) {
    log_config = instr->parent->parent->module->log_config;
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Erasing instruction with opcode %d from block %s", instr->opcode,
              instr->parent && instr->parent->label ? instr->parent->label
                                                    : "unknown");
  }

  // 前提条件：要删除的指令的结果值不能再被任何其他指令使用。
  if (instr->dest) {
    assert(instr->dest->use_list_head == NULL &&
           "Cannot erase instruction whose result is still in use!");
  }

  if (instr->parent && instr->parent->parent) {
    instr->parent->parent->instruction_count--;
  }

  // 断开此指令与其所有操作数之间的Use-Def链接
  while (instr->operand_head) {
    remove_operand(instr->operand_head);
  }

  // 从基本块的指令双向链表中解开
  if (instr->parent) {
    if (instr->prev) {
      instr->prev->next = instr->next;
    } else {
      instr->parent->head = instr->next;
    }
    if (instr->next) {
      instr->next->prev = instr->prev;
    } else {
      instr->parent->tail = instr->prev;
    }
  }

  // 标记为已死，防止意外重用
  instr->opcode = IR_OP_UNKNOWN;
  instr->parent = NULL;
  instr->dest = NULL;
  instr->in_worklist = false;
}

/**
 * @brief 标记一条指令为待删除（用于延迟删除）。
 */
void mark_instruction_for_removal(IRInstruction *instr) {
  if (!instr)
    return;
  instr->opcode = IR_OP_UNKNOWN;
}

/**
 * @brief 清理并真正删除所有被标记为待删除的指令。
 */
void cleanup_removed_instructions(IRBasicBlock *block) {
  if (!block)
    return;

  IRInstruction *instr = block->head;
  while (instr) {
    IRInstruction *next = instr->next;
    if (instr->opcode == IR_OP_UNKNOWN) {
      erase_instruction(instr);
    }
    instr = next;
  }
}

/******************************************************************************
 *                                                                            *
 *                   4. CFG, PHI, Dominator Tree 工具函数 *
 *                                                                            *
 ******************************************************************************/

/**
 * @brief (内部函数) 移除PHI指令中的一对操作数（值和块）。
 */
static void remove_phi_operand_pair(IROperand *op_val) {
  if (!op_val)
    return;

  IROperand *op_block = op_val->next_in_instr;
  assert(op_block &&
         "Malformed PHI operand pair: value is not followed by a block.");

  remove_operand(op_val);
  remove_operand(op_block);
}

/**
 * @brief 从一个基本块的前驱列表中移除一个指定的前驱，并同步更新所有PHI指令。
 */
void remove_predecessor(IRBasicBlock *block, IRBasicBlock *pred_to_remove) {
  // 记录前驱移除操作
  LogConfig *log_config = NULL;
  if (block && block->parent && block->parent->module) {
    log_config = block->parent->module->log_config;
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Removing predecessor %s from block %s",
              pred_to_remove->label ? pred_to_remove->label : "unnamed",
              block->label ? block->label : "unnamed");
  }

  int found_idx = -1;
  for (int i = 0; i < block->num_predecessors; ++i) {
    if (block->predecessors[i] == pred_to_remove) {
      found_idx = i;
      break;
    }
  }
  if (found_idx == -1)
    return;

  // 从动态数组中移除
  for (int i = found_idx; i < block->num_predecessors - 1; ++i) {
    block->predecessors[i] = block->predecessors[i + 1];
  }
  block->num_predecessors--;

  // 遍历并修复所有PHI指令
  for (IRInstruction *instr = block->head; instr && instr->opcode == IR_OP_PHI;
       instr = instr->next) {
    remove_phi_entries_for_predecessor(instr, pred_to_remove);
  }
}

/**
 * @brief 从一个基本块的后继列表中移除一个指定的后继。
 */
void remove_successor(IRBasicBlock *block, IRBasicBlock *succ_to_remove) {
  int found_idx = -1;
  for (int i = 0; i < block->num_successors; ++i) {
    if (block->successors[i] == succ_to_remove) {
      found_idx = i;
      break;
    }
  }
  if (found_idx == -1)
    return;

  for (int i = found_idx; i < block->num_successors - 1; ++i) {
    block->successors[i] = block->successors[i + 1];
  }
  block->num_successors--;
}

/**
 * @brief 向一个基本块的后继列表中添加一个新的后继。
 */
void add_successor(IRBasicBlock *block, IRBasicBlock *new_succ) {
  if (block->num_successors >= block->capacity_successors) {
    int new_capacity =
        (block->capacity_successors == 0) ? 4 : block->capacity_successors * 2;
    MemoryPool *pool = block->parent->module->pool;
    IRBasicBlock **new_succs = (IRBasicBlock **)pool_alloc(
        pool, new_capacity * sizeof(IRBasicBlock *));
    if (block->successors) {
      memcpy(new_succs, block->successors,
             block->num_successors * sizeof(IRBasicBlock *));
    }
    block->successors = new_succs;
    block->capacity_successors = new_capacity;
  }
  block->successors[block->num_successors++] = new_succ;
}

/**
 * @brief 向一个基本块的前驱列表中添加一个新的前驱。
 */
void add_predecessor(IRBasicBlock *block, IRBasicBlock *new_pred) {
  if (block->num_predecessors >= block->capacity_predecessors) {
    int new_capacity = (block->capacity_predecessors == 0)
                           ? 4
                           : block->capacity_predecessors * 2;
    MemoryPool *pool = block->parent->module->pool;
    IRBasicBlock **new_preds = (IRBasicBlock **)pool_alloc(
        pool, new_capacity * sizeof(IRBasicBlock *));
    if (block->predecessors) {
      memcpy(new_preds, block->predecessors,
             block->num_predecessors * sizeof(IRBasicBlock *));
    }
    block->predecessors = new_preds;
    block->capacity_predecessors = new_capacity;
  }
  block->predecessors[block->num_predecessors++] = new_pred;
}

/**
 * @brief 改变终结符指令（如br）的一个目标基本块。
 */
void change_terminator_target(IRInstruction *term, IRBasicBlock *from,
                              IRBasicBlock *to) {
  assert(is_terminator_instruction(term) && "Instruction is not a terminator.");
  for (IROperand *op = term->operand_head; op; op = op->next_in_instr) {
    if (op->kind == IR_OP_KIND_BASIC_BLOCK && op->data.bb == from) {
      op->data.bb = to;
    }
  }
}

/**
 * @brief 重定向一条CFG边，并同步更新所有相关数据结构。
 * @param from 边的起始块。
 * @param old_to 边的原目标块。
 * @param new_to 边的新目标块。
 */
void redirect_edge(IRBasicBlock *from, IRBasicBlock *old_to,
                   IRBasicBlock *new_to) {
  if (!from || !old_to || !new_to || from == old_to || from == new_to) {
    return;
  }

  // 记录CFG边重定向操作
  LogConfig *log_config = NULL;
  if (from && from->parent && from->parent->module) {
    log_config = from->parent->module->log_config;
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Redirecting edge from %s: %s -> %s",
              from->label ? from->label : "unnamed",
              old_to->label ? old_to->label : "unnamed",
              new_to->label ? new_to->label : "unnamed");
  }

  // 1. 更新 'from' 块的终结符指令，将所有指向 old_to 的操作数改为指向 new_to
  if (from->tail && is_terminator_instruction(from->tail)) {
    change_terminator_target(from->tail, old_to, new_to);
  }

  // 2. 更新 'from' 的后继列表
  remove_successor(from, old_to);
  add_successor(from, new_to);

  // 3. 更新 'old_to' 和 'new_to' 的前驱列表
  remove_predecessor(old_to, from);
  add_predecessor(new_to, from);

  // 注意：此函数不负责修复PHI节点，这是调用者的责任。
  // 调用者应在之后显式调用 `repair_phi_nodes_after_edge_redirect`。
}

/**
 * @brief 在CFG边重定向后修复PHI节点。
 */
void repair_phi_nodes_after_edge_redirect(IRBasicBlock *new_to,
                                          IRBasicBlock *from,
                                          IRBasicBlock *old_to) {
  if (!new_to || !from || !old_to)
    return;
  MemoryPool *pool = new_to->parent->module->pool;

  // 1. 修复 new_to 的 PHI 节点
  for (IRInstruction *phi_new = new_to->head;
       phi_new && phi_new->opcode == IR_OP_PHI; phi_new = phi_new->next) {
    // 通过 phi_for_alloca 精确匹配 old_to 中的 PHI
    IRInstruction *phi_old = NULL;
    if (phi_new->phi_for_alloca) {
      for (IRInstruction *p = old_to->head; p && p->opcode == IR_OP_PHI;
           p = p->next) {
        if (p->phi_for_alloca == phi_new->phi_for_alloca) {
          phi_old = p;
          break;
        }
      }
    }

    bool has_from = false;
    for (IROperand *op_val = phi_new->operand_head;
         op_val && op_val->next_in_instr;
         op_val = op_val->next_in_instr->next_in_instr) {
      if (op_val->next_in_instr->data.bb == from) {
        has_from = true;
        break;
      }
    }

    if (!has_from) {
      IRValue *incoming_val = NULL;
      if (phi_old) {
        incoming_val = phi_get_incoming_value_for_block(phi_old, from);
      }
      if (!incoming_val) {
        incoming_val = get_undef_value(phi_new->dest->type, pool);
      }
      add_value_operand(phi_new, incoming_val);
      add_bb_operand(phi_new, from);
    }
  }

  // 2. 移除 old_to 的 PHI 节点中 from 的入口
  for (IRInstruction *phi = old_to->head; phi && phi->opcode == IR_OP_PHI;
       phi = phi->next) {
    remove_phi_entries_for_predecessor(phi, from);
  }
}

/**
 * @brief 获取指定类型的全局undef值（未定义值）。
 */
IRValue *get_undef_value(Type *type, MemoryPool *pool) {
  // 简单实现：每次都新建一个undef对象。
  // 优化：可以为每种类型缓存一个唯一的undef值。
  IRValue *v = create_ir_value(pool);
  v->is_constant = true; // undef 在语义上是常量
  v->type = type;
  // v->is_undef = true; // 可选的标志位
  return v;
}

/**
 * @brief 改变一个基本块中所有PHI指令对应于某个前驱的入口。
 */
void change_phi_predecessor(IRBasicBlock *block, IRBasicBlock *from,
                            IRBasicBlock *to) {
  for (IRInstruction *instr = block->head; instr && instr->opcode == IR_OP_PHI;
       instr = instr->next) {
    // PHI 指令的操作数总是成对出现：[value, block], [value, block], ...
    for (IROperand *op = instr->operand_head; op && op->next_in_instr;
         op = op->next_in_instr->next_in_instr) {
      IROperand *block_op = op->next_in_instr;
      assert(block_op->kind == IR_OP_KIND_BASIC_BLOCK);
      if (block_op->data.bb == from) {
        block_op->data.bb = to;
      }
    }
  }
}

/**
 * @brief 检查一个基本块是否支配另一个基本块。
 */
bool dominates(IRBasicBlock *dom, IRBasicBlock *use) {
  if (dom == use)
    return true;
  if (!dom || !use)
    return false;

  // 使用预先计算的时间戳进行 O(1) 的支配查询。
  // dom支配use，当且仅当dom的进入时间早于等于use的进入时间，且dom的离开时间晚于等于use的离开时间。
  return dom->dom_tin <= use->dom_tin && dom->dom_tout >= use->dom_tout;
}

/**
 * @brief 获取PHI指令中来自指定基本块的值。
 */
IRValue *phi_get_incoming_value_for_block(IRInstruction *phi,
                                          IRBasicBlock *block) {
  if (!phi || phi->opcode != IR_OP_PHI || !block)
    return NULL;

  // PHI 操作数成对出现: [value1, block1], [value2, block2], ...
  for (IROperand *op_val = phi->operand_head; op_val && op_val->next_in_instr;
       op_val = op_val->next_in_instr->next_in_instr) {
    IROperand *op_block = op_val->next_in_instr;
    if (op_block->kind == IR_OP_KIND_BASIC_BLOCK &&
        op_block->data.bb == block) {
      return op_val->data.value;
    }
  }
  return NULL;
}

/**
 * @brief 从PHI指令中移除所有来自指定前驱的入口。
 */
void remove_phi_entries_for_predecessor(IRInstruction *phi,
                                        IRBasicBlock *pred) {
  if (!phi || phi->opcode != IR_OP_PHI || !pred)
    return;

  IROperand *op_val = phi->operand_head;
  while (op_val) {
    IROperand *op_block = op_val->next_in_instr;
    // 在删除前安全地获取下一个要检查的对的起始操作数
    IROperand *next_op_val = op_block ? op_block->next_in_instr : NULL;

    if (op_block && op_block->kind == IR_OP_KIND_BASIC_BLOCK &&
        op_block->data.bb == pred) {
      remove_phi_operand_pair(op_val); // 这个调用会删除 op_val 和 op_block
    }

    op_val = next_op_val;
  }
}

/**
 * @brief 在指定基本块之后插入新的基本块到函数的块链表中。
 */
void insert_block_after(IRBasicBlock *new_bb, IRBasicBlock *pos) {
  if (!new_bb || !pos || !pos->parent) {
    assert(0 && "Invalid arguments for insert_block_after");
    return;
  }

  IRFunction *func = pos->parent;
  new_bb->parent = func;
  new_bb->next_in_func = pos->next_in_func;
  new_bb->prev_in_func = pos;

  if (pos->next_in_func) {
    pos->next_in_func->prev_in_func = new_bb;
  } else {
    assert(func->tail == pos);
    func->tail = new_bb;
  }

  pos->next_in_func = new_bb;
  func->block_count++;
}

// 定义用于 qsort 的比较函数
static int compare_loops_by_depth(const void *a, const void *b) {
  const Loop *loop1 = *(const Loop **)a;
  const Loop *loop2 = *(const Loop **)b;

  int depth1 = 0;
  for (const Loop *p = loop1->parent; p; p = p->parent) {
    depth1++;
  }

  int depth2 = 0;
  for (const Loop *p = loop2->parent; p; p = p->parent) {
    depth2++;
  }

  if (depth2 > depth1)
    return 1; // 深度大的（内层）排前面
  if (depth2 < depth1)
    return -1; // 深度小的（外层）排后面

  // 深度相同，按后序ID排序以保证稳定性
  if (loop1->header->post_order_id < loop2->header->post_order_id)
    return 1;
  if (loop1->header->post_order_id > loop2->header->post_order_id)
    return -1;

  return 0;
}

/**
 * @brief 收集函数中所有循环并按深度排序（从内层到外层）。
 */
Worklist *get_loops_sorted_by_depth(IRFunction *func) {
  if (!func || !func->top_level_loops)
    return create_worklist(func->module->pool, 0);

  MemoryPool *pool = func->module->pool;
  Worklist *all_loops = create_worklist(pool, func->block_count);
  Worklist *temp_worklist = create_worklist(pool, func->block_count);

  // 1. 使用工作列表进行广度优先或深度优先遍历，收集所有循环
  for (Loop *loop = func->top_level_loops; loop; loop = loop->next) {
    worklist_add(temp_worklist, loop);
  }

  while (temp_worklist->count > 0) {
    Loop *loop = (Loop *)worklist_pop(temp_worklist);
    worklist_add(all_loops, loop);

    for (int i = 0; i < loop->num_sub_loops; ++i) {
      worklist_add(temp_worklist, loop->sub_loops[i]);
    }
  }

  // 2. 使用 qsort 进行高效排序
  if (all_loops->count > 1) {
    qsort(all_loops->items, all_loops->count, sizeof(void *),
          compare_loops_by_depth);
  }

  return all_loops;
}

/**
 * @brief 重新计算函数中的指令总数（在修改IR后调用）。
 */
void recalculate_instruction_count(IRFunction *func) {
  if (!func)
    return;

  func->instruction_count = 0;
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    for (IRInstruction *instr = bb->head; instr; instr = instr->next) {
      if (instr->opcode != IR_OP_UNKNOWN) {
        func->instruction_count++;
      }
    }
  }
}

/**
 * @brief 比较两个类型是否相同。
 */
bool is_type_same(Type *t1, Type *t2, bool strict) {
  if (t1 == t2)
    return true;
  if (!t1 || !t2)
    return false;
  if (t1->kind != t2->kind)
    return false;
  if (strict && t1->is_const != t2->is_const)
    return false;

  switch (t1->kind) {
  case TYPE_BASIC:
    return t1->basic == t2->basic;
  case TYPE_POINTER:
    return is_type_same(t1->pointer.element_type, t2->pointer.element_type,
                        strict);
  case TYPE_ARRAY:
    if (t1->array.dim_count != t2->array.dim_count)
      return false;
    if (!is_type_same(t1->array.element_type, t2->array.element_type, false))
      return false;
    for (size_t i = 0; i < t1->array.dim_count; ++i) {
      if (t1->array.dimensions[i].is_dynamic !=
          t2->array.dimensions[i].is_dynamic)
        return false;
      if (!t1->array.dimensions[i].is_dynamic &&
          t1->array.dimensions[i].static_size !=
              t2->array.dimensions[i].static_size) {
        return false;
      }
    }
    return true;
  case TYPE_FUNCTION:
    if (!is_type_same(t1->function.return_type, t2->function.return_type,
                      strict))
      return false;
    if (t1->function.param_count != t2->function.param_count)
      return false;
    for (size_t i = 0; i < t1->function.param_count; ++i) {
      if (!is_type_same(t1->function.param_types[i],
                        t2->function.param_types[i], strict))
        return false;
    }
    return t1->function.is_variadic == t2->function.is_variadic;
  default:
    return false;
  }
}

/**
 * @brief 简单深拷贝一条IR指令。
 */
IRInstruction *clone_instruction(IRInstruction *instr, MemoryPool *pool) {
  if (!instr)
    return NULL;

  IRInstruction *new_instr = create_ir_instruction(instr->opcode, pool);

  // 复制非指针、非链表的简单字段
  new_instr->opcode_cond = pool_strdup(pool, instr->opcode_cond);
  new_instr->align = instr->align;
  new_instr->is_inbounds = instr->is_inbounds;

  // 深拷贝操作数链表，但不链接Use-Def链
  for (IROperand *op = instr->operand_head; op; op = op->next_in_instr) {
    add_operand(new_instr, op->kind,
                (op->kind == IR_OP_KIND_VALUE) ? (void *)op->data.value
                                               : (void *)op->data.bb);
  }

  // dest需要由调用者重新分配和设置
  new_instr->dest = NULL;

  return new_instr;
}

/**
 * @brief 重新设计 clone_instruction 函数，接受 IRBuilder 和 ValueMap 参数。
 * @details 它应该接受一个 IRBuilder（用于创建新寄存器）和一个
 * ValueMap（用于重映射操作数）， 并返回一个功能完整的克隆指令。
 */
IRInstruction *clone_instruction_with_remap(IRInstruction *instr,
                                            IRBuilder *builder,
                                            ValueMap *remap) {
  if (!instr)
    return NULL;

  MemoryPool *pool = builder->module->pool;
  IRInstruction *new_instr = create_ir_instruction(instr->opcode, pool);

  // 1. 复制非指针、非链表的简单字段
  new_instr->opcode_cond = pool_strdup(pool, instr->opcode_cond);
  new_instr->align = instr->align;
  new_instr->is_inbounds = instr->is_inbounds;

  // 2. 创建新的 dest 寄存器 (如果原指令有的话)
  if (instr->dest) {
    // 使用 builder 创建一个新寄存器，保证名称唯一
    new_instr->dest =
        ir_builder_create_reg(builder, instr->dest->type, "clone");
    new_instr->dest->def_instr = new_instr;

    // 在重映射表中记录：旧的 dest 寄存器 -> 新的 dest 寄存器
    // 这样，后续克隆的指令如果使用了这个旧 dest，就会被自动重映射
    if (remap) {
      value_map_put(remap, instr->dest, new_instr->dest,
                    builder->module->log_config);
    }
  }

  // 3. 克隆并重映射所有操作数
  for (IROperand *op = instr->operand_head; op; op = op->next_in_instr) {
    if (op->kind == IR_OP_KIND_VALUE) {
      // 从重映射表中查找新值，如果找不到则使用原值
      IRValue *remapped_val = remap_value(remap, op->data.value);
      add_value_operand(new_instr, remapped_val);
    } else { // IR_OP_KIND_BASIC_BLOCK
      // 基本块通常不需要重映射，直接复制
      // (如果优化涉及到块的克隆，则也需要对块进行重映射)
      add_bb_operand(new_instr, op->data.bb);
    }
  }

  return new_instr;
}

/**
 * @brief 使用ValueMap重映射指令的所有值操作数。
 */
void remap_instruction_operands(IRInstruction *instr, ValueMap *value_remap) {
  if (!instr || !value_remap)
    return;

  // 获取日志配置（从指令的模块中获取）
  LogConfig *log_config = NULL;
  if (instr->parent && instr->parent->parent && instr->parent->parent->module &&
      instr->parent->parent->module->log_config) {
    log_config = instr->parent->parent->module->log_config;
  }

  int remapped_count = 0;
  for (IROperand *op = instr->operand_head; op; op = op->next_in_instr) {
    if (op->kind == IR_OP_KIND_VALUE) {
      IRValue *old_val = op->data.value;
      IRValue *new_val = remap_value(value_remap, old_val);
      if (new_val != old_val) {
        remapped_count++;
        if (log_config) {
          LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
                    "Remapping operand in instruction %s: %s -> %s",
                    instr->opcode_cond ? instr->opcode_cond : "unknown",
                    old_val->name ? old_val->name : "unnamed",
                    new_val->name ? new_val->name : "unnamed");
        }
      }
      // 注意：change_operand_value 会处理 Use-Def 链的更新
      change_operand_value(op, new_val);
    }
  }

  if (log_config && remapped_count > 0) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Remapped %d operands in instruction %s", remapped_count,
              instr->opcode_cond ? instr->opcode_cond : "unknown");
  }
}

/**
 * @brief 断开一个基本块的所有后继（用于CFG重连）。
 */
void sever_all_successors(IRBasicBlock *bb) {
  if (!bb)
    return;

  // 获取日志配置
  LogConfig *log_config = NULL;
  if (bb->parent && bb->parent->module && bb->parent->module->log_config) {
    log_config = bb->parent->module->log_config;
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Severing all successors from block %s (has %d successors)",
              bb->label ? bb->label : "unnamed", bb->num_successors);
  }

  if (bb->tail) {
    erase_instruction(bb->tail);
  }

  for (int i = 0; i < bb->num_successors; ++i) {
    remove_predecessor(bb->successors[i], bb);
  }
  bb->num_successors = 0;

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Successfully severed all successors from block %s",
              bb->label ? bb->label : "unnamed");
  }
}

/**
 * @brief 将src映射表合并到dst，若key已存在则覆盖。
 */
void value_map_merge(ValueMap *dst, ValueMap *src) {
  if (!dst || !src)
    return;

  // 由于无法直接访问MemoryPool的module字段，我们暂时使用NULL
  // 在实际使用中，调用者应该传递正确的LogConfig
  LogConfig *log_config = NULL;

  for (int i = 0; i < src->count; ++i) {
    value_map_put(dst, src->entries[i].old_val, src->entries[i].new_val,
                  log_config);
  }
}

/**
 * @brief (内部函数) 对支配树进行深度优先搜索，以计算时间戳。
 */
static void dfs_dom_tree(IRBasicBlock *bb, int *time) {
  if (!bb)
    return;
  bb->dom_tin = (*time)++;
  for (int i = 0; i < bb->dom_children_count; ++i) {
    dfs_dom_tree(bb->dom_children[i], time);
  }
  bb->dom_tout = (*time)++;
}

/**
 * @brief 计算函数内支配树的时间戳，用于O(1)支配查询。
 */
void compute_dom_tree_timestamps(IRFunction *func) {
  if (!func || !func->entry)
    return;
  int time = 0;
  dfs_dom_tree(func->entry, &time);
}

/**
 * @brief 将所有对基本块'from'的引用，替换为对'to'的引用。
 */
void replace_all_uses_with_block(IRBasicBlock *from, IRBasicBlock *to) {
  if (!from || !to || from == to)
    return;

  IRFunction *func = from->parent;
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    for (IRInstruction *instr = bb->head; instr; instr = instr->next) {
      for (IROperand *op = instr->operand_head; op; op = op->next_in_instr) {
        if (op->kind == IR_OP_KIND_BASIC_BLOCK && op->data.bb == from) {
          op->data.bb = to;
        }
      }
    }
  }
}

/**
 * @brief 从函数的块链表中移除一个基本块。
 */
void remove_block_from_function(IRBasicBlock *bb) {
  if (!bb || !bb->parent)
    return;
  IRFunction *func = bb->parent;

  // 获取日志配置
  LogConfig *log_config = NULL;
  if (func->module && func->module->log_config) {
    log_config = func->module->log_config;
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Removing block %s from function %s (function had %d blocks)",
              bb->label ? bb->label : "unnamed",
              func->name ? func->name : "unnamed", func->block_count);
  }

  if (bb->prev_in_func) {
    bb->prev_in_func->next_in_func = bb->next_in_func;
  } else {
    func->blocks = bb->next_in_func;
  }

  if (bb->next_in_func) {
    bb->next_in_func->prev_in_func = bb->prev_in_func;
  } else {
    func->tail = bb->prev_in_func;
  }

  bb->parent = NULL;
  func->block_count--;

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Successfully removed block %s from function %s (function now "
              "has %d blocks)",
              bb->label ? bb->label : "unnamed",
              func->name ? func->name : "unnamed", func->block_count);
  }
}

/**
 * @brief 将 from 块的所有指令移动到 to 块的末尾。
 */
void move_instructions_to_block_end(IRBasicBlock *from, IRBasicBlock *to) {
  if (!from || !to || from == to || !from->head)
    return;

  // 获取日志配置
  LogConfig *log_config = NULL;
  if (from->parent && from->parent->module &&
      from->parent->module->log_config) {
    log_config = from->parent->module->log_config;
  }

  // 计算要移动的指令数量
  int instruction_count = 0;
  for (IRInstruction *instr = from->head; instr; instr = instr->next) {
    instruction_count++;
  }

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Moving %d instructions from block %s to block %s",
              instruction_count, from->label ? from->label : "unnamed",
              to->label ? to->label : "unnamed");
  }

  // 将 from 的指令链表拼接到 to 的末尾
  if (!to->head) {
    to->head = from->head;
    to->tail = from->tail;
  } else {
    to->tail->next = from->head;
    from->head->prev = to->tail;
    to->tail = from->tail;
  }

  // 更新所有移动指令的 parent 指针
  for (IRInstruction *instr = from->head; instr; instr = instr->next) {
    instr->parent = to;
  }

  // 清空 from 块
  from->head = from->tail = NULL;

  if (log_config) {
    LOG_DEBUG(log_config, LOG_CATEGORY_IR_OPT,
              "Successfully moved %d instructions from block %s to block %s",
              instruction_count, from->label ? from->label : "unnamed",
              to->label ? to->label : "unnamed");
  }
}

/**
 * @brief 获取循环内所有指令的拓扑排序（此处为简单的线性收集）。
 */
Worklist *get_instructions_in_loop_topo_order(Loop *loop) {
  if (!loop || !loop->blocks)
    return NULL;
  Worklist *wl = create_worklist(loop->blocks[0]->parent->module->pool, 64);

  // 遍历循环的所有基本块
  for (int i = 0; i < loop->num_blocks; ++i) {
    IRBasicBlock *bb = loop->blocks[i];
    // 遍历块内所有指令
    for (IRInstruction *instr = bb->head; instr; instr = instr->next) {
      worklist_add(wl, instr);
    }
  }
  return wl;
}

/**
 * @brief (IRBuilder辅助) 根据操作码创建二元操作指令。
 */
IRInstruction *ir_builder_create_binary_op(IRBuilder *builder, Opcode op,
                                           IRValue *lhs, IRValue *rhs,
                                           const char *name) {
  switch (op) {
  case IR_OP_ADD:
    return ir_builder_create_add(builder, lhs, rhs, name);
  case IR_OP_SUB:
    return ir_builder_create_sub(builder, lhs, rhs, name);
  case IR_OP_MUL:
    return ir_builder_create_mul(builder, lhs, rhs, name);
  case IR_OP_SDIV:
    return ir_builder_create_sdiv(builder, lhs, rhs, name);
  case IR_OP_SREM:
    return ir_builder_create_srem(builder, lhs, rhs, name);
  case IR_OP_FADD:
    return ir_builder_create_fadd(builder, lhs, rhs, name);
  case IR_OP_FSUB:
    return ir_builder_create_fsub(builder, lhs, rhs, name);
  case IR_OP_FMUL:
    return ir_builder_create_fmul(builder, lhs, rhs, name);
  case IR_OP_FDIV:
    return ir_builder_create_fdiv(builder, lhs, rhs, name);
  default:
    return NULL;
  }
}