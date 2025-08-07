/**
 * @file loop_analysis.c
 * @brief 实现函数CFG中的自然循环分析。
 * @details
 * 本文件实现了在控制流图上识别自然循环的算法。它能：
 * 1.  **识别循环**: 通过查找支配其尾部的循环头（即回边）来识别所有自然循环。
 * 2.  **收集循环体**:
 * 对每个循环，通过反向CFG遍历，精确地收集所有属于该循环的基本块。
 * 3.  **构建循环层级**: 分析循环之间的嵌套关系，构建循环树。
 * 4.  **计算循环信息**:
 * 为每个循环计算出口块、回边等信息，为后续循环优化提供支持。
 * 此分析依赖于预先计算好的CFG和支配树信息。
 */
#include "ir/analysis/loop_analysis.h"
#include "ast.h"
#include "ir/ir_utils.h" // For BitSet, Worklist, and dominates
#include "logger.h"
#include <assert.h>
#include <string.h>

// --- 辅助函数原型声明 ---
static void collect_loop_body(Loop *loop);
static void build_loop_hierarchy(IRFunction *func, Loop **all_loops,
                                 int loop_count);
static void compute_exit_blocks(Loop *loop);
static void add_block_to_loop(Loop *loop, IRBasicBlock *bb);
static void add_back_edge_to_loop(Loop *loop, IRBasicBlock *back_edge_src);

// --- 主入口点 ---
Loop *find_loops(IRFunction *func) {
  if (!func || !func->entry || !func->reverse_post_order) {
    if (func)
      func->top_level_loops = NULL;
    return NULL;
  }

  if (func->module && func->module->log_config) {
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN,
              "Finding loops in function @%s", func->name);
  }

  assert(func->module && func->module->pool);
  MemoryPool *pool = func->module->pool;
  int block_count = func->block_count;

  // header_map 用于通过基本块的ID快速查找其作为头部的循环。
  Loop **header_map = (Loop **)pool_alloc_z(pool, block_count * sizeof(Loop *));

  // 收集所有找到的循环
  Loop **all_loops = (Loop **)pool_alloc(pool, block_count * sizeof(Loop *));
  int loop_count = 0;

  // 1. 查找所有回边（back-edge），以识别循环及其头部。
  // 回边是一条从节点 N 指向其支配者 D 的边。
  for (int i = 0; i < block_count; ++i) {
    IRBasicBlock *bb_n = func->reverse_post_order[i];
    for (int j = 0; j < bb_n->num_successors; ++j) {
      IRBasicBlock *bb_d_header = bb_n->successors[j];

      if (dominates(bb_d_header, bb_n)) { // N->D 是一条回边
        Loop *loop = header_map[bb_d_header->post_order_id];
        if (!loop) {
          // 如果是第一次发现以此块为头的循环，则创建一个新的 Loop 结构。
          loop = (Loop *)pool_alloc_z(pool, sizeof(Loop));
          loop->header = bb_d_header;
          loop->loop_blocks_bs = bitset_create(block_count, pool);

          header_map[bb_d_header->post_order_id] = loop;
          all_loops[loop_count++] = loop;

          add_block_to_loop(loop, loop->header);
        }
        // 将回边的源节点（N）添加到循环的回边列表中。
        add_back_edge_to_loop(loop, bb_n);
      }
    }
  }

  if (loop_count == 0) {
    LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN,
              "No loops found in @%s", func->name);
    func->top_level_loops = NULL;
    return NULL;
  }

  // 2. 对每个已识别的循环，收集其所有的基本块。
  for (int i = 0; i < loop_count; ++i) {
    collect_loop_body(all_loops[i]);
  }

  // 3. 从 BitSet 最终确定循环块列表，并计算出口块。
  for (int i = 0; i < loop_count; ++i) {
    Loop *loop = all_loops[i];

    loop->blocks = (IRBasicBlock **)pool_alloc(
        pool, loop->num_blocks * sizeof(IRBasicBlock *));
    int current_block_idx = 0;
    // 以逆后序遍历，可能会有更好的缓存局部性。
    for (int j = 0; j < block_count; ++j) {
      IRBasicBlock *bb = func->reverse_post_order[j];
      if (bitset_contains(loop->loop_blocks_bs, bb->post_order_id)) {
        loop->blocks[current_block_idx++] = bb;
        bb->loop_depth++; // 更新块的循环嵌套深度
      }
    }
    assert(current_block_idx == loop->num_blocks);
    compute_exit_blocks(loop);
  }

  // 4. 构建循环之间的父子嵌套关系。
  build_loop_hierarchy(func, all_loops, loop_count);

  // 5. 将顶层循环链接起来，并存储到函数对象中。
  func->top_level_loops = NULL;
  for (int i = 0; i < loop_count; ++i) {
    if (all_loops[i]->parent == NULL) {
      all_loops[i]->next = func->top_level_loops;
      func->top_level_loops = all_loops[i];
    }
  }

  LOG_DEBUG(func->module->log_config, LOG_CATEGORY_IR_GEN,
            "Found %d total loops in @%s", loop_count, func->name);
  return func->top_level_loops;
}

// --- 辅助函数实现 ---

/** @brief 将一个块添加到循环中（如果尚未存在）。*/
static void add_block_to_loop(Loop *loop, IRBasicBlock *bb) {
  if (bitset_contains(loop->loop_blocks_bs, bb->post_order_id))
    return;

  // 检查BitSet边界（在添加前记录日志）
  if (bb->post_order_id >= loop->loop_blocks_bs->num_words * 64) {
    // 获取LogConfig从模块中
    LogConfig *log_config = NULL;
    if (bb->parent && bb->parent->module) {
      log_config = bb->parent->module->log_config;
    }
    if (log_config) {
      LOG_WARN(log_config, LOG_CATEGORY_IR_OPT,
               "BitSet boundary violation: block ID %d exceeds capacity %d in "
               "loop analysis",
               bb->post_order_id, loop->loop_blocks_bs->num_words * 64);
    }
  }

  bitset_add(loop->loop_blocks_bs, bb->post_order_id, loop->header->parent->module->log_config);
  loop->num_blocks++;
}

/** @brief 将一个回边的源节点添加到循环的回边列表中。*/
static void add_back_edge_to_loop(Loop *loop, IRBasicBlock *back_edge_src) {
  // 动态扩容回边数组
  if (loop->num_back_edges >= loop->capacity_back_edges) {
    int old_capacity = loop->capacity_back_edges;
    loop->capacity_back_edges = old_capacity > 0 ? old_capacity * 2 : 4;
    MemoryPool *pool = loop->header->parent->module->pool;
    IRBasicBlock **new_list = (IRBasicBlock **)pool_alloc(
        pool, loop->capacity_back_edges * sizeof(IRBasicBlock *));
    if (loop->back_edges) {
      memcpy(new_list, loop->back_edges,
             loop->num_back_edges * sizeof(IRBasicBlock *));
    }
    loop->back_edges = new_list;
  }
  loop->back_edges[loop->num_back_edges++] = back_edge_src;
}

/** @brief 从回边开始，通过反向CFG遍历来收集构成循环体的所有块。*/
static void collect_loop_body(Loop *loop) {
  MemoryPool *pool = loop->header->parent->module->pool;
  Worklist *wl = create_worklist(pool, 16);

  // 用所有回边的源节点作为工作列表的初始种子。
  for (int i = 0; i < loop->num_back_edges; ++i) {
    IRBasicBlock *back_edge_src = loop->back_edges[i];
    add_block_to_loop(loop, back_edge_src);
    worklist_add(wl, back_edge_src);
  }

  // 从种子开始，沿着前驱边反向遍历，直到遇到循环头或已在循环内的块。
  while (wl->count > 0) {
    IRBasicBlock *current = (IRBasicBlock *)worklist_pop(wl);
    for (int i = 0; i < current->num_predecessors; ++i) {
      IRBasicBlock *pred = current->predecessors[i];

      // 如果前驱尚未被添加到循环体中，则添加它并放入工作列表。
      if (!bitset_contains(loop->loop_blocks_bs, pred->post_order_id)) {
        add_block_to_loop(loop, pred);
        worklist_add(wl, pred);
      }
    }
  }
}

/** @brief 计算一个循环的所有出口块。*/
static void compute_exit_blocks(Loop *loop) {
  MemoryPool *pool = loop->header->parent->module->pool;
  int block_count = loop->header->parent->block_count;

  // 使用 BitSet 来避免重复添加同一个出口块。
  BitSet *exit_block_bs = bitset_create(block_count, pool);

  // 临时存储出口块指针。
  IRBasicBlock **temp_exits =
      (IRBasicBlock **)pool_alloc(pool, block_count * sizeof(IRBasicBlock *));
  int exit_count = 0;

  // 遍历循环内的所有块
  for (int i = 0; i < loop->num_blocks; ++i) {
    IRBasicBlock *bb = loop->blocks[i];
    // 检查它的每一个后继
    for (int j = 0; j < bb->num_successors; ++j) {
      IRBasicBlock *succ = bb->successors[j];
      // 如果一个后继不在循环体内，那么它就是一个出口块。
      if (!bitset_contains(loop->loop_blocks_bs, succ->post_order_id)) {
        if (!bitset_contains(exit_block_bs, succ->post_order_id)) {
          bitset_add(exit_block_bs, succ->post_order_id, loop->header->parent->module->log_config);
          temp_exits[exit_count++] = succ;
        }
      }
    }
  }

  // 将临时的出口块列表复制到最终大小的数组中。
  loop->num_exit_blocks = exit_count;
  if (exit_count > 0) {
    loop->exit_blocks =
        (IRBasicBlock **)pool_alloc(pool, exit_count * sizeof(IRBasicBlock *));
    memcpy(loop->exit_blocks, temp_exits, exit_count * sizeof(IRBasicBlock *));
  } else {
    loop->exit_blocks = NULL;
  }
}

/** @brief 构建循环的嵌套层级关系。*/
static void build_loop_hierarchy(IRFunction *func, Loop **all_loops,
                                 int loop_count) {
  // 按循环包含的块数量升序排序。这样可以确保在处理一个循环时，它可能包含的
  // 所有子循环都已经被处理过了。
  for (int i = 0; i < loop_count - 1; ++i) {
    for (int j = 0; j < loop_count - i - 1; ++j) {
      if (all_loops[j]->num_blocks > all_loops[j + 1]->num_blocks) {
        Loop *temp = all_loops[j];
        all_loops[j] = all_loops[j + 1];
        all_loops[j + 1] = temp;
      }
    }
  }

  // 对每个循环 L1，查找包含它的最小的循环 L2。
  for (int i = 0; i < loop_count; ++i) {
    Loop *l1 = all_loops[i];
    for (int j = i + 1; j < loop_count; ++j) {
      Loop *l2 = all_loops[j];
      // 如果 L2 的块集合包含了 L1 的头部，那么 L2 就是 L1 的一个父循环。
      // 因为我们是按大小排序的，所以找到的第一个就是最紧密的父循环。
      if (bitset_contains(l2->loop_blocks_bs, l1->header->post_order_id)) {
        l1->parent = l2;

        // 将 l1 添加到 l2 的子循环列表中。
        if (l2->num_sub_loops >= l2->capacity_sub_loops) {
          int old_capacity = l2->capacity_sub_loops;
          l2->capacity_sub_loops = old_capacity > 0 ? old_capacity * 2 : 4;
          Loop **new_list = (Loop **)pool_alloc(
              func->module->pool, l2->capacity_sub_loops * sizeof(Loop *));
          if (l2->sub_loops) {
            memcpy(new_list, l2->sub_loops, l2->num_sub_loops * sizeof(Loop *));
          }
          l2->sub_loops = new_list;
        }
        l2->sub_loops[l2->num_sub_loops++] = l1;
        break;
      }
    }
  }
}