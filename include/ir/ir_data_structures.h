#ifndef IR_DATA_STRUCTURES_H
#define IR_DATA_STRUCTURES_H

#include "ast.h"
#include "logger.h"
#include <stdbool.h>
#include <stddef.h> // For size_t
#include <stdint.h>

// 前向声明来自其他模块的类型
typedef struct Type Type;
typedef struct MemoryPool MemoryPool;

// 前向声明来自本模块的类型，解决循环引用问题
typedef struct IRValue IRValue;
typedef struct IRInstruction IRInstruction;
typedef struct IRBasicBlock IRBasicBlock;
typedef struct IRFunction IRFunction;
typedef struct IRModule IRModule;
typedef struct IRGlobalVariable IRGlobalVariable;
typedef struct IROperand IROperand;
typedef struct Loop Loop;

/**
 * @enum OperandKind
 * @brief 指令操作数的种类。
 * @details
 * 用于区分一个操作数是引用一个值（如寄存器或常量），还是引用一个基本块（如分支指令的目标）。
 */
typedef enum {
  IR_OP_KIND_VALUE,       ///< 操作数是一个 `IRValue*`（寄存器、常量）
  IR_OP_KIND_BASIC_BLOCK, ///< 操作数是一个 `IRBasicBlock*`（标签）
} OperandKind;

/**
 * @enum Opcode
 * @brief IR 指令的操作码枚举。
 */
typedef enum {
  // 终结符指令 (Terminators)
  IR_OP_RET,
  IR_OP_BR,
  // 二元整数运算 (Binary Ops)
  IR_OP_ADD,
  IR_OP_SUB,
  IR_OP_MUL,
  IR_OP_SDIV,
  IR_OP_SREM,
  // 二元浮点运算
  IR_OP_FADD,
  IR_OP_FSUB,
  IR_OP_FMUL,
  IR_OP_FDIV,
  // 位运算
  IR_OP_SHL,  // 逻辑左移
  IR_OP_LSHR, // 逻辑右移
  IR_OP_ASHR, // 算术右移
  IR_OP_AND,
  IR_OP_OR,
  IR_OP_XOR,
  // 内存操作 (Memory Ops)
  IR_OP_ALLOCA,
  IR_OP_LOAD,
  IR_OP_STORE,
  IR_OP_GETELEMENTPTR,
  // 其他指令
  IR_OP_ICMP,
  IR_OP_FCMP,
  IR_OP_PHI,
  IR_OP_CALL,
  // 类型转换指令
  IR_OP_SITOFP,
  IR_OP_FPTOSI,
  IR_OP_ZEXT,
  IR_OP_FPEXT,
  IR_OP_SEXT,
  IR_OP_TRUNC,
  IR_OP_FPTRUNC, // 新增 SEXT 用于 i32->i64, TRUNC 用于截断, FPTRUNC
                 // 用于浮点截断
  // 未知或占位指令
  IR_OP_UNKNOWN,
} Opcode;

/**
 * @struct ConstantAggregate
 * @brief 表示一个聚合常量，例如常量数组的初始化列表。
 */
typedef struct ConstantAggregate {
  IRValue **elements; ///< 指向常量元素值数组的指针
  size_t count;       ///< 元素数量
} ConstantAggregate;

/**
 * @struct IRValue
 * @brief 表示 IR 中的一个"值"。
 * @details 这是 IR 中最基本的概念之一，任何可以作为指令操作数的东西都是一个值，
 *          包括常量、指令的计算结果（虚拟寄存器）、函数参数、全局变量等。
 */
struct IRValue {
  bool is_constant; ///< 标记此值是否为常量
  bool is_global;   ///< 标记此值是否为全局符号（如全局变量、函数），用于IR打印
  union {
    int int_val;       ///< 整型常量的值
    int64_t i64_val;   ///< 64位整型常量的值
    float float_val;   ///< 浮点型常量的值
    double double_val; ///< 双精度浮点型常量的值
    char *name;        ///< 值的名称（如虚拟寄存器名 `%1`，函数名 `@foo`）
    ConstantAggregate aggregate; ///< 聚合常量的值
  };
  Type *type; ///< 此值的类型（例如 `i32`, `float`, `i32*` 等）

  // 如果此值是一个指令的结果（虚拟寄存器），此指针指向定义它的那条指令。
  // 对于其他值（如全局变量、常量、函数参数），此字段为 NULL。
  IRInstruction *def_instr;
  IRInstruction *alloca_instr; // 如果此值是参数，指向其 alloca 指令

  // Use-Def/Def-Use
  // 链的核心：指向使用此值的所有操作数（IROperand）形成的单向链表的头部。
  // 通过遍历此链表，可以找到所有使用当前值的指令。
  IROperand *use_list_head;
};

/**
 * @struct IROperand
 * @brief 表示指令的一个操作数。
 * @details 这是一个关键的连接结构，它既属于某条指令（作为其操作数之一），
 *          也属于某个值（作为其使用者之一），从而构成了 Use-Def/Def-Use 链。
 */
struct IROperand {
  OperandKind kind; ///< 操作数的种类（值或基本块）
  union {
    IRValue *value; ///< 如果 kind 是 `IR_OP_KIND_VALUE`，指向引用的值
    IRBasicBlock
        *bb; ///< 如果 kind 是 `IR_OP_KIND_BASIC_BLOCK`，指向引用的基本块
  } data;

  IRInstruction *user; ///< 指向使用此操作数的指令

  // 用于形成指令内部操作数列表的双向链表指针
  struct IROperand *next_in_instr;
  struct IROperand *prev_in_instr;

  // 用于形成值的 Use 链表的单向链表指针
  struct IROperand *next_use;
};

/**
 * @struct IRInstruction
 * @brief 表示 IR 中的一条指令。
 */
struct IRInstruction {
  Opcode opcode;     ///< 指令的操作码
  char *opcode_cond; ///< 用于 `icmp` 和 `fcmp` 的条件码字符串 (如 "eq", "slt")
  IRValue *dest; ///< 指令计算结果存放的目标值（虚拟寄存器），如果指令无返回值则为
                 ///< NULL

  IROperand *operand_head; ///< 指向该指令操作数链表的头部
  IROperand *operand_tail; ///< 指向该指令操作数链表的尾部（用于高效插入）
  int num_operands;        ///< 操作数数量

  IRBasicBlock *parent;       ///< 指向包含此指令的基本块
  IRInstruction *next, *prev; ///< 用于链接基本块中所有指令的双向链表指针

  // 用于 mem2reg 优化的特殊标记，将一个 PHI 节点关联到它所替代的 alloca 指令。
  struct IRInstruction *phi_for_alloca;

  // --- 用于保证代码质量和正确性的属性 ---
  int align;        ///< 用于内存操作的对齐字节数
  bool is_inbounds; ///< 用于 GEP 指令，标记地址计算是否保证在边界内
  bool in_worklist; ///< 用于优化器的工作列表，避免重复添加
  bool is_live;     ///< 用于死代码消除，标记指令是否为活跃的
};

/**
 * @struct IRBasicBlock
 * @brief 表示一个基本块（Basic Block）。
 * @details
 * 基本块是一系列指令的线性序列，只有一个入口（第一条指令）和一个出口（最后一条终结符指令）。
 */
struct IRBasicBlock {
  char *label;                ///< 基本块的标签名
  IRInstruction *head, *tail; ///< 指向块内指令链表的头和尾
  IRFunction *parent;         ///< 指向包含此基本块的函数

  // 用于形成函数内部基本块列表的双向链表指针（按代码布局顺序）
  IRBasicBlock *prev_in_func;
  IRBasicBlock *next_in_func;

  // --- 控制流图（CFG）分析结果 ---
  IRBasicBlock **successors; ///< 后继基本块数组
  int num_successors;
  int capacity_successors;
  IRBasicBlock **predecessors; ///< 前驱基本块数组
  int num_predecessors;
  int capacity_predecessors;

  // --- 支配树（Dominator Tree）分析结果 ---
  int post_order_id;           ///< 后序遍历ID，用于分析
  int dom_tin;                 ///< 进入支配树节点的时间戳
  int dom_tout;                ///< 离开支配树节点的时间戳
  IRBasicBlock *idom;          ///< 直接支配者（Immediate Dominator）
  IRBasicBlock **dom_frontier; ///< 支配边界（Dominance Frontier）集合
  int dom_frontier_count;

  IRBasicBlock **dom_children; ///< 在支配树中的子节点
  int dom_children_count;

  // --- 循环分析结果 ---
  int loop_depth; ///< 循环嵌套深度
};

/**
 * @struct IRFunction
 * @brief 表示一个函数。
 */
struct IRFunction {
  char *name;            ///< 函数名
  Type *return_type;     ///< 函数返回类型
  IRBasicBlock *entry;   ///< 函数的入口基本块
  IRBasicBlock *blocks;  ///< 指向函数内基本块链表的头部
  IRBasicBlock *tail;    ///< 指向函数内基本块链表的尾部
  int block_count;       ///< 函数内的基本块数量
  int instruction_count; ///< 函数内的指令总数（缓存值）
  IRFunction *next;      ///< 用于链接模块中所有函数的链表指针

  // --- 新增参数值列表 ---
  IRValue **args; ///< 参数值列表
  int num_args;   ///< 参数数量

  // --- 缓存的分析结果 ---
  IRBasicBlock *
      *reverse_post_order; ///< 基本块的逆后序（RPO）列表，由支配分析计算得出
  Loop *top_level_loops;   ///< 指向该函数内顶层循环链表的头部

  IRModule *module; ///< 指向包含此函数的模块
};

/**
 * @struct IRGlobalVariable
 * @brief 表示模块中的一个全局变量。
 */
typedef struct IRGlobalVariable {
  char *name;                    ///< 全局变量的名称（例如 `@g`）
  bool is_const;                 ///< 是否为全局常量
  Type *type;                    ///< 全局变量的类型
  IRValue *initializer;          ///< 指向其初始化值（可以是简单常量或聚合常量）
  struct IRGlobalVariable *next; ///< 用于链接模块中所有全局变量的链表指针
} IRGlobalVariable;

/**
 * @struct Loop
 * @brief 表示一个循环结构。
 * @details 用于存储循环分析的结果，包括循环的基本块、嵌套关系等信息。
 */
struct Loop {
  IRBasicBlock *header; ///< 循环头（支配所有循环内基本块）
  IRBasicBlock
      *preheader; ///< 循环前置头（支配循环头且唯一后继是循环头的基本块）
  struct BitSet *loop_blocks_bs; ///< 循环内基本块的位集合
  IRBasicBlock **blocks;         ///< 循环内基本块数组
  int num_blocks;                ///< 循环内基本块数量

  IRBasicBlock **back_edges; ///< 回边源节点数组
  int num_back_edges;        ///< 回边数量
  int capacity_back_edges;   ///< 回边数组容量

  IRBasicBlock **exit_blocks; ///< 循环出口块数组
  int num_exit_blocks;        ///< 出口块数量

  struct Loop *parent;     ///< 父循环（如果此循环嵌套在另一个循环内）
  struct Loop **sub_loops; ///< 子循环数组
  int num_sub_loops;       ///< 子循环数量
  int capacity_sub_loops;  ///< 子循环数组容量

  struct Loop *next; ///< 用于链接同级循环的链表指针
};

/**
 * @struct IRModule
 * @brief 表示一个 IR 模块。
 */
struct IRModule {
  char *source_filename;     ///< 源文件名
  IRFunction *functions;     ///< 指向模块中函数链表的头部
  IRGlobalVariable *globals; ///< 指向模块中全局变量链表的头部

  LogConfig *
      log_config; ///< 指向日志配置的指针，用于在整个IR系统中保持日志配置的一致性

  MemoryPool *pool; ///< 用于此模块所有IR对象分配的内存池
};

#endif // IR_DATA_STRUCTURES_H