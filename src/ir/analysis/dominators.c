/**
 * @file dominators.c
 * @brief 支配树和支配边界分析的实现。
 * @details
 * 本文件实现了计算控制流图中节点间支配关系的算法。它包括：
 * 1.  **支配集计算**: 通过迭代数据流分析，为每个基本块找到所有支配它的块。
 * 2.  **直接支配者 (IDom) 计算**: 根据支配集确定每个块唯一的直接支配者。
 * 3.  **支配树构建**: 根据直接支配者关系构建支配树结构。
 * 4.  **支配边界 (Dominance Frontier) 计算**: 计算每个块的支配边界，这是构建
 * SSA 形式的关键。
 * 5.  **时间戳生成**: 为支配树节点生成时间戳，以支持 O(1) 的快速支配关系查询。
 * 算法依赖于预先构建好的 CFG，并使用 BitSet 进行高效的集合运算。
 */
#include "ir/analysis/dominators.h"
#include "ast.h"
#include "ir/ir_utils.h" // For BitSet, compute_dom_tree_timestamps
#include "logger.h"
#include <assert.h>
#include <stdint.h> // for uint64_t
#include <string.h>

// --- 辅助数据结构 ---

// 上下文结构，用于持有单个函数分析过程中的所有数据。
typedef struct {
  IRFunction *func;
  MemoryPool *pool;
  IRBasicBlock **post_order;         // 后序遍历列表
  IRBasicBlock **reverse_post_order; // 逆后序遍历列表
  int block_count;
  BitSet **dom_sets; // 支配集数组（每个元素是一个指向BitSet的指针）
} DominatorContext;

// --- 分析步骤的原型声明 ---
static void perform_post_order_traversal(DominatorContext *ctx);
static void compute_dominator_sets(DominatorContext *ctx);
static void compute_immediate_dominators(DominatorContext *ctx);
static void compute_dominance_frontiers(DominatorContext *ctx);
static void build_dominator_tree(DominatorContext *ctx);

// --- 主驱动函数 ---
void compute_dominators(IRFunction *func) {
  if (!func || !func->entry) {
    return;
  }

  if (func->module && func->module->log_config) {
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN,
              "Computing dominators for function @%s", func->name);
  }

  assert(func->module && func->module->pool);
  MemoryPool *pool = func->module->pool;

  // --- 1. 初始化 ---
  DominatorContext ctx;
  ctx.func = func;
  ctx.pool = pool;
  ctx.block_count = func->block_count;

  ctx.post_order = (IRBasicBlock **)pool_alloc(
      pool, ctx.block_count * sizeof(IRBasicBlock *));
  ctx.reverse_post_order = (IRBasicBlock **)pool_alloc(
      pool, ctx.block_count * sizeof(IRBasicBlock *));
  ctx.dom_sets =
      (BitSet **)pool_alloc(pool, ctx.block_count * sizeof(BitSet *));

  // 为每个基本块分配一个临时的、唯一的ID，用于本次分析。
  int id_counter = 0;
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    bb->post_order_id = id_counter++;
  }

  // 为每个块初始化一个 BitSet。
  for (id_counter = 0; id_counter < ctx.block_count; ++id_counter) {
    ctx.dom_sets[id_counter] = bitset_create(ctx.block_count, pool);
  }

  // --- 2. 运行分析遍 ---
  perform_post_order_traversal(&ctx); // 计算后序和逆后序
  compute_dominator_sets(&ctx);       // 计算每个块的支配集
  compute_immediate_dominators(&ctx); // 根据支配集计算直接支配者
  build_dominator_tree(&ctx);         // 根据直接支配者关系构建支配树
  compute_dominance_frontiers(&ctx);  // 根据支配树计算支配边界

  // 计算时间戳以支持 O(1) 的支配查询
  compute_dom_tree_timestamps(func);

  // --- 3. 清理 ---
  // 所有内存都由内存池管理，无需显式释放。
  if (func->module && func->module->log_config) {
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN,
              "Dominator analysis for @%s complete.", func->name);
  }
}

// --- 分析步骤的实现 ---

/** @brief 深度优先搜索访问函数，用于后序遍历。*/
static void dfs_visit(IRBasicBlock *block, bool *visited,
                      IRBasicBlock **post_order_list, int *count) {
  visited[block->post_order_id] = true;
  for (int i = 0; i < block->num_successors; ++i) {
    IRBasicBlock *succ = block->successors[i];
    if (!visited[succ->post_order_id]) {
      dfs_visit(succ, visited, post_order_list, count);
    }
  }
  post_order_list[(*count)++] = block;
}

/** @brief 执行后序遍历，并生成后序和逆后序列表。*/
static void perform_post_order_traversal(DominatorContext *ctx) {
  bool *visited =
      (bool *)pool_alloc_z(ctx->pool, ctx->block_count * sizeof(bool));
  int count = 0;

  dfs_visit(ctx->func->entry, visited, ctx->post_order, &count);
  assert(count == ctx->block_count && "Post-order traversal did not visit all "
                                      "blocks. CFG might be disconnected.");

  // 根据后序列表生成逆后序列表，并更新块的 post_order_id
  for (int i = 0; i < ctx->block_count; ++i) {
    ctx->post_order[i]->post_order_id = i;
    ctx->reverse_post_order[ctx->block_count - 1 - i] = ctx->post_order[i];
  }

  // 将结果缓存到函数对象中
  ctx->func->reverse_post_order = ctx->reverse_post_order;
}

/** @brief 使用迭代数据流分析计算每个基本块的支配集。*/
static void compute_dominator_sets(DominatorContext *ctx) {
  IRBasicBlock *entry = ctx->func->entry;
  int entry_id = entry->post_order_id;

  // Dom(入口块) = {入口块}
  // 检查BitSet边界（在添加前记录日志）
  if (entry_id >= ctx->dom_sets[entry_id]->num_words * 64) {
    LogConfig *log_config = ctx->func->module->log_config;
    if (log_config) {
      LOG_WARN(log_config, LOG_CATEGORY_IR_OPT,
               "BitSet boundary violation: entry block ID %d exceeds capacity "
               "%d in dominator analysis",
               entry_id, ctx->dom_sets[entry_id]->num_words * 64);
    }
  }

  bitset_add(ctx->dom_sets[entry_id], entry_id, ctx->func->module->log_config);

  // 对于所有其他节点 n, Dom(n) 初始化为 {所有节点}
  for (int i = 0; i < ctx->block_count; ++i) {
    IRBasicBlock *bb = ctx->post_order[i];
    if (bb != entry) {
      bitset_set_all(ctx->dom_sets[bb->post_order_id], ctx->block_count);
    }
  }

  bool changed = true;
  BitSet *temp_set = bitset_create(ctx->block_count, ctx->pool);
  while (changed) {
    changed = false;
    // 以逆后序遍历，可以加快收敛速度
    for (int i = 0; i < ctx->block_count; ++i) {
      IRBasicBlock *b = ctx->reverse_post_order[i];
      if (b == entry)
        continue;

      int b_id = b->post_order_id;

      // new_dom = Intersect(Dom(p))，对所有前驱 p
      if (b->num_predecessors > 0) {
        bitset_copy(temp_set, ctx->dom_sets[b->predecessors[0]->post_order_id]);
        for (int j = 1; j < b->num_predecessors; ++j) {
          bitset_intersect(temp_set,
                           ctx->dom_sets[b->predecessors[j]->post_order_id]);
        }
      } else {
        // 不可达块的支配集为空
        memset(temp_set->words, 0, temp_set->num_words * sizeof(uint64_t));
      }

      // Dom(B) = {B} U new_dom
      bitset_add(temp_set, b_id, ctx->func->module->log_config);

      // 检查集合是否改变，以判断是否达到不动点
      if (!bitset_equals(ctx->dom_sets[b_id], temp_set)) {
        bitset_copy(ctx->dom_sets[b_id], temp_set);
        changed = true;
      }
    }
  }
}

/** @brief 根据支配集计算每个块的直接支配者。*/
static void compute_immediate_dominators(DominatorContext *ctx) {
  for (int i = 0; i < ctx->block_count; ++i) {
    IRBasicBlock *b = ctx->reverse_post_order[i];
    b->idom = NULL; // 初始化 idom
    if (b == ctx->func->entry) {
      continue;
    }

    int b_id = b->post_order_id;
    BitSet *b_doms = ctx->dom_sets[b_id];
    int idom_id = -1;

    // 直接支配者是严格支配 b 的所有块中，后序遍历序号最高的那一个。
    for (int dom_candidate_id = 0; dom_candidate_id < ctx->block_count;
         ++dom_candidate_id) {
      if (dom_candidate_id == b_id)
        continue;

      if (bitset_contains(b_doms, dom_candidate_id)) {
        if (idom_id == -1 || dom_candidate_id > idom_id) {
          idom_id = dom_candidate_id;
        }
      }
    }

    assert(idom_id != -1 &&
           "Could not find an immediate dominator for a non-entry block.");

    // 使用后序列表进行 O(1) 的ID到指针的转换
    assert(idom_id >= 0 && idom_id < ctx->block_count);
    b->idom = ctx->post_order[idom_id];
  }
}

/** @brief 根据直接支配者关系构建支配树（填充 dom_children 列表）。*/
static void build_dominator_tree(DominatorContext *ctx) {
  IRFunction *func = ctx->func;
  MemoryPool *pool = ctx->pool;

  // Pass 1: 计算每个节点的子节点数量
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    bb->dom_children_count = 0;
    bb->dom_children = NULL;
  }
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    if (bb->idom) {
      bb->idom->dom_children_count++;
    }
  }

  // Pass 2: 为子节点数组分配内存
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    if (bb->dom_children_count > 0) {
      bb->dom_children = (IRBasicBlock **)pool_alloc(
          pool, bb->dom_children_count * sizeof(IRBasicBlock *));
      bb->dom_children_count = 0; // 重置为索引
    }
  }

  // Pass 3: 填充子节点数组
  for (IRBasicBlock *bb = func->blocks; bb; bb = bb->next_in_func) {
    if (bb->idom) {
      IRBasicBlock *parent = bb->idom;
      parent->dom_children[parent->dom_children_count++] = bb;
    }
  }
}

/** @brief 根据支配树计算支配边界。*/
static void compute_dominance_frontiers(DominatorContext *ctx) {
  MemoryPool *pool = ctx->pool;

  // 创建临时的 BitSet 数组来存储支配边界
  BitSet **df_sets =
      (BitSet **)pool_alloc(pool, ctx->block_count * sizeof(BitSet *));
  for (int i = 0; i < ctx->block_count; ++i) {
    df_sets[i] = bitset_create(ctx->block_count, pool);
  }

  // Phase 1: 使用经典的支配边界算法计算
  for (int i = 0; i < ctx->block_count; ++i) {
    IRBasicBlock *b = ctx->reverse_post_order[i];
    if (b->num_predecessors < 2)
      continue; // 只有汇聚点才有非空的局部支配边界

    for (int j = 0; j < b->num_predecessors; ++j) {
      IRBasicBlock *p = b->predecessors[j];
      IRBasicBlock *runner = p;

      while (runner != b->idom) {
        assert(runner != NULL && "Runner reached root without hitting idom(b). "
                                 "CFG may be malformed.");
        // 将 b 添加到 runner 的支配边界中
        bitset_add(df_sets[runner->post_order_id], b->post_order_id, ctx->func->module->log_config);
        runner = runner->idom; // 沿着支配树向上走
      }
    }
  }

  // Phase 2: 将 BitSet 转换为最终的数组表示
  for (int i = 0; i < ctx->block_count; ++i) {
    IRBasicBlock *b = ctx->post_order[i];
    BitSet *df_set = df_sets[i];

    int count = 0;
    for (int id = 0; id < ctx->block_count; ++id) {
      if (bitset_contains(df_set, id)) {
        count++;
      }
    }

    if (count > 0) {
      b->dom_frontier_count = count;
      b->dom_frontier =
          (IRBasicBlock **)pool_alloc(pool, count * sizeof(IRBasicBlock *));
      int current_idx = 0;

      for (int id = 0; id < ctx->block_count; ++id) {
        if (bitset_contains(df_set, id)) {
          b->dom_frontier[current_idx++] = ctx->post_order[id];
        }
      }
    } else {
      b->dom_frontier_count = 0;
      b->dom_frontier = NULL;
    }
  }
}