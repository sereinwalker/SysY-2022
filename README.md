# SysY 编译器前端详细文档

## 目录
1. [概述](#概述)
2. [项目结构](#项目结构)
3. [扫描器上下文 (Scanner Context)](#扫描器上下文-scanner-context)
4. [词法分析器 (Lexer)](#词法分析器-lexer)
5. [语义分析器 (Semantic Analyzer)](#语义分析器-semantic-analyzer)
6. [抽象语法树 (AST)](#抽象语法树-ast)
7. [符号表管理](#符号表管理)
8. [错误处理机制](#错误处理机制)
9. [编译流程](#编译流程)
10. [技术特点](#技术特点)

## 1. 概述

本文档详细介绍了本 SysY 编译器的前端部分。前端负责将 SysY 源代码作为输入，经过一系列处理，最终生成一种中间表示（通常是抽象语法树 AST），并附带丰富的语义信息。这个过程为后续的后端（代码优化和目标代码生成）奠定了坚实的基础。

本编译器前端主要由以下几个核心模块构成：

## 项目结构

```
src/
├── lexer/              # 词法分析器
│   └── lexer.l         # Flex 词法规则文件
├── parser/             # 语法分析器
│   ├── parser.y        # Bison 语法规则文件
│   └── parser_driver.c # 解析器驱动程序
├── semantic_analyzer/  # 语义分析器
│   └── semantic_analyzer.c
├── ast/                # 抽象语法树定义
├── symbol_table/       # 符号表管理
├── scanner/            # 扫描器上下文
└── utils/              # 工具函数
```

## 扫描器上下文 (Scanner Context)

### 文件位置
- `src/scanner/scanner_context.h`
- `src/scanner/scanner_context.c`

### 功能概述
`ScannerContext` 是连接词法分析器、语法分析器和语义分析器的核心“胶水”组件。它并非一个独立的编译阶段，而是一个贯穿整个前端生命周期的**上下文管理器**，旨在支持一个可重入的、线程安全的编译流程。

### 主要职责

1.  **中心状态管理**: 由编译器驱动程序创建，并作为参数传递给词法、语法和语义分析的各个阶段。它持有所有阶段所需的共享状态。
2.  **核心资源容器**: 它内部封装并管理着 `ASTContext`，这意味着它间接掌管着**内存池**和**错误收集器**。所有模块都通过 `ScannerContext` 来访问这些核心资源。
3.  **生命周期控制**: `create_scanner_context` 和 `destroy_scanner_context` 函数分别负责初始化和释放所有前端资源，确保了清晰的资源生命周期和无内存泄漏。
4.  **词法分析支持**: 为词法分析器提供了状态存储，例如一个动态增长的缓冲区 (`string_buffer`)，用于在扫描过程中拼接字符串字面量。

## 词法分析器 (Lexer)

### 文件位置
- `src/lexer/lexer.l`

### 功能概述
词法分析器使用 **Flex** 工具实现，负责将 SysY 源代码分解为词法单元 (tokens)。

### 主要特性

#### 1. 支持的词法单元类型
- **关键字**: `const`, `int`, `float`, `void`, `if`, `else`, `while`, `break`, `continue`, `return`
- **标识符**: 以字母或下划线开头的标识符序列
- **常量**:
  - 整数常量：十进制、八进制 (0开头)、十六进制 (0x开头)
  - 浮点常量：支持科学计数法和十六进制浮点数
- **字符串字面量**: 支持转义序列
- **运算符**: 算术、关系、逻辑、赋值运算符
- **分隔符**: 括号、大括号、方括号、分号、逗号

#### 2. 注释处理
```c
// 单行注释支持
/* 多行注释支持 */
```

#### 3. 字符串处理
- 支持转义序列：`\n`, `\t`, `\r`, `\"`, `\\`
- 检测未终止的字符串字面量
- 处理非法转义序列

#### 4. 错误处理
- 整数溢出检测
- 浮点数溢出检测
- 非法字符检测
- 未终止注释检测

#### 5. 位置跟踪
```c
#define UPDATE_LOCATION() \
    do { \
        YY_LLOC_P->first_line = YY_LLOC_P->last_line; \
        YY_LLOC_P->first_column = YY_LLOC_P->last_column; \
        for (int i = 0; yytext[i] != '\0'; i++) { \
            if (yytext[i] == '\n') { \
                YY_LLOC_P->last_line++; \
                YY_LLOC_P->last_column = 1; \
            } else { \
                YY_LLOC_P->last_column++; \
            } \
        } \
    } while(0)
```

### 技术实现
- **可重入设计**: 使用 `%option reentrant` 支持多线程
- **Bison 集成**: 通过 `%option bison-bridge bison-locations` 与语法分析器集成
- **状态机**: 使用 `%x COMMENT` 和 `%x STRING` 处理复杂的词法状态

## 语法分析器 (Parser)

### 文件位置
- `src/parser/parser.y`
- `src/parser/parser_driver.c`

### 功能概述
语法分析器使用 **Bison** 工具实现，采用 LALR(1) 解析算法，将词法单元流转换为抽象语法树。

### 主要特性

#### 1. 语法规则覆盖
- **编译单元**: 全局声明和函数定义
- **声明**: 常量声明、变量声明、函数声明
- **语句**: 赋值、表达式、控制流语句
- **表达式**: 算术、关系、逻辑表达式，支持运算符优先级

#### 2. 运算符优先级
```yacc
%left OR
%left AND
%left EQ NE
%left LT LE GT GE
%left ADD SUB
%left MUL DIV MOD
%right NOT UMINUS
%nonassoc IF_WITHOUT_ELSE
%nonassoc ELSE
```

#### 3. 数组支持
- 多维数组声明
- 数组初始化列表
- 数组访问表达式

#### 4. 函数支持
- 函数定义和声明
- 参数列表处理
- 函数调用表达式

#### 5. 错误恢复
```yacc
block_item_list: block_item_list error SEMICOLON { yyerrok; }
```

### 内存管理
- **内存池**: 使用自定义内存池管理 AST 节点
- **列表管理**: 动态扩展的节点列表结构
- **可重入安全**: 所有状态通过 `ScannerContext` 传递

## 语义分析器 (Semantic Analyzer)

### 文件位置
- `src/semantic_analyzer/semantic_analyzer.c`

### 功能概述
语义分析器对 AST 进行两遍遍历，完成符号表构建和语义检查。

### 主要功能

#### 1. 符号表构建 (第一遍)
- **作用域管理**: 全局、函数、块级作用域
- **符号注册**: 变量、常量、函数符号
- **重定义检查**: 检测同一作用域内的符号重定义
- **标准库函数**: 预定义标准库函数符号

```c
static const char *reserved_stdlib_names[] = {
    "getint", "getch", "getfloat", "getarray", "getfarray",
    "putint", "putch", "putfloat", "putarray", "putfarray",
    "putf", "starttime", "stoptime", NULL
};
```

#### 2. 语义检查 (第二遍)
- **类型检查**: 赋值兼容性、运算符类型匹配
- **符号解析**: 标识符使用前必须声明
- **控制流分析**: 
  - 非 void 函数返回值检查
  - break/continue 语句位置检查
  - 不可达代码检测
- **常量折叠**: 编译时常量表达式求值

#### 3. 数组语义
- **维度检查**: 数组维度必须为正整数常量
- **初始化检查**: 数组初始化列表验证
- **边界分析**: 静态数组边界检查

#### 4. 函数语义
- **参数匹配**: 函数调用参数类型和数量检查
- **返回类型**: 返回值类型兼容性检查
- **递归检测**: 函数递归调用分析

### 类型系统

#### 基本类型
```c
typedef enum {
    BASIC_INT,
    BASIC_FLOAT,
    BASIC_VOID
} BasicType;
```

#### 复合类型
- **数组类型**: 支持多维数组，包含维度信息
- **函数类型**: 包含参数类型和返回类型
- **常量修饰**: 支持 const 修饰符

#### 类型兼容性
- **隐式转换**: int ↔ float 自动转换
- **数组兼容**: 数组参数传递的特殊规则
- **常量兼容**: const 修饰符的兼容性规则

### 错误检测

#### 溢出检查
```c
bool would_add_overflow(int a, int b);
bool would_sub_overflow(int a, int b);
bool would_mul_overflow(int a, int b);
bool would_div_overflow(int a, int b);
```

#### 范围检查
- 整数常量范围验证
- 浮点数精度检查
- 数组索引边界检查

## 抽象语法树 (AST)

### 节点类型
AST 支持丰富的节点类型，涵盖 SysY 语言的所有语法结构：

#### 声明节点
- `AST_CONST_DECL`: 常量声明
- `AST_VAR_DECL`: 变量声明
- `AST_FUNC_DECL`: 函数声明
- `AST_PARAM_DECL`: 参数声明

#### 语句节点
- `AST_ASSIGN_STMT`: 赋值语句
- `AST_EXPR_STMT`: 表达式语句
- `AST_IF_STMT`: 条件语句
- `AST_WHILE_STMT`: 循环语句
- `AST_BREAK_STMT`: break 语句
- `AST_CONTINUE_STMT`: continue 语句
- `AST_RETURN_STMT`: return 语句
- `AST_BLOCK`: 代码块

#### 表达式节点
- `AST_BINARY_EXPR`: 二元表达式
- `AST_UNARY_EXPR`: 一元表达式
- `AST_CALL_EXPR`: 函数调用
- `AST_ARRAY_ACCESS`: 数组访问
- `AST_IDENTIFIER`: 标识符
- `AST_INT_CONSTANT`: 整数常量
- `AST_FLOAT_CONSTANT`: 浮点常量
- `AST_STRING_LITERAL`: 字符串字面量

### 节点属性
每个 AST 节点包含：
- **节点类型**: 标识节点的语法类别
- **位置信息**: 源代码中的行列位置
- **类型信息**: 表达式的求值类型
- **符号引用**: 指向符号表中的符号
- **子节点**: 指向子 AST 节点的指针

## 符号表管理

### 作用域层次
- **全局作用域**: 全局变量和函数
- **函数作用域**: 函数参数和局部变量
- **块作用域**: 代码块内的局部变量

### 符号属性
```c
typedef struct Symbol {
    char* name;                    // 符号名称
    Type* type;                    // 符号类型
    SymbolKind kind;               // 符号种类
    SourceLocation loc;            // 声明位置
    bool is_evaluated;             // 是否已求值
    ConstValueUnion const_val;     // 常量值
    struct Symbol* next;           // 链表指针
} Symbol;
```

### 符号查找
- **层次查找**: 从当前作用域向外层查找
- **符号遮蔽**: 内层符号遮蔽外层同名符号
- **前向声明**: 支持函数前向声明

## 错误处理机制

### 错误分类
```c
typedef enum {
    ERROR_LEXICAL,      // 词法错误
    ERROR_SYNTAX,       // 语法错误
    ERROR_SEMANTIC,     // 语义错误
    ERROR_TYPE_MISMATCH,// 类型不匹配
    ERROR_UNDECLARED,   // 未声明标识符
    ERROR_REDEFINITION, // 重定义错误
    // ... 更多错误类型
} ErrorType;
```

### 错误报告
- **位置信息**: 精确的行列位置
- **错误描述**: 详细的错误信息
- **错误恢复**: 语法分析器的错误恢复机制
- **错误统计**: 错误和警告计数

### 错误处理策略
- **继续分析**: 遇到错误后继续分析，发现更多错误
- **级联错误**: 避免因单个错误引起的大量级联错误
- **友好提示**: 提供有用的错误修复建议

## 编译流程

编译器前端的工作流程以 `ScannerContext` 的生命周期为核心，串联起各个模块：

1.  **初始化**: 编译器驱动程序调用 `create_scanner_context(filename)` 创建一个 `ScannerContext` 实例。此函数会完成所有前端子系统的初始化，包括创建 `ASTContext`（进而创建内存池和错误收集器）。

2.  **词法与语法分析**: 驱动程序初始化 `flex` 扫描器 (`yylex_init`) 和 `bison` 解析器，并将 `ScannerContext` 实例注入其中。随后调用 `yyparse(ctx)` 启动解析。
    -   **词法分析器** (`lexer.l`) 在执行时，通过 `YY_CTX` 宏访问 `ScannerContext`，以使用其内部的字符串缓冲区等资源。
    -   **语法分析器** (`parser.y`) 在归约产生式时，通过 `ctx` 参数访问 `ASTContext`，调用内存池中的工厂函数来构建**抽象语法树 (AST)**。

3.  **语义分析**: 语法分析成功后，驱动程序从 `ScannerContext` 中获取到完整的 AST，并调用 `perform_semantic_analysis`。
    -   语义分析器对 AST 进行两遍遍历，通过传入的上下文访问符号表和类型系统，完成符号构建和类型检查。

4.  **资源清理与报告**: 
    -   无论编译成功与否，驱动程序最后都会调用 `destroy_scanner_context(ctx)`。该函数会按照创建的逆序，安全地释放内存池、错误列表、文件名副本等所有资源。
    -   在清理前，可以从 `ScannerContext` 中提取错误信息并格式化输出给用户。

5.  **交付成果**: 如果前端处理成功（没有致命错误），最终生成的、附带了完整语义信息的 AST 将被传递给编译器后端。

## 技术特点

### 1. 可重入设计
- 词法分析器和语法分析器都支持可重入
- 线程安全的实现
- 状态通过上下文结构传递

### 2. 内存管理
- 自定义内存池，提高分配效率
- 统一的内存清理机制
- 避免内存泄漏

### 3. 错误处理
- 完善的错误分类和报告
- 多阶段错误检测
- 友好的错误信息

### 4. 扩展性
- 模块化设计，易于扩展
- 清晰的接口定义
- 支持新的语言特性添加

### 5. 性能优化
- 高效的符号表查找
- 常量折叠优化
- 内存池分配策略

### 6. 标准兼容
- 严格遵循 SysY 语言规范
- 支持标准库函数
- 兼容性良好的类型系统

## 总结

本 SysY 编译器前端是一个功能完整、设计良好的编译器前端实现。它采用了经典的编译器设计模式，具有以下优点：

1. **结构清晰**: 三阶段设计，职责分明
2. **功能完整**: 支持 SysY 语言的所有特性
3. **错误处理**: 完善的错误检测和报告机制
4. **性能良好**: 高效的算法和数据结构
5. **可维护性**: 模块化设计，代码组织良好
6. **扩展性**: 易于添加新特性和优化

该前端为后续的中间代码生成、优化和目标代码生成提供了坚实的基础。
