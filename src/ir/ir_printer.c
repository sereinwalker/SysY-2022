#include "ast.h"
#include "ir/ir_data_structures.h"
#include <stdio.h>

/**
 * @file ir_printer.c
 * @brief 实现将内存中的IR结构打印为文本格式的功能，用于调试和输出。
 */

/**
 * @brief 将操作码枚举转换为对应的字符串表示。
 * @param opcode 要转换的操作码。
 * @return 操作码的字符串形式。
 */
static const char* opcode_to_string(Opcode opcode) {
    switch (opcode) {
        case IR_OP_RET: return "ret";
        case IR_OP_BR: return "br";
        case IR_OP_ADD: return "add";
        case IR_OP_SUB: return "sub";
        case IR_OP_MUL: return "mul";
        case IR_OP_SDIV: return "sdiv";
        case IR_OP_SREM: return "srem";
        case IR_OP_FADD: return "fadd";
        case IR_OP_FSUB: return "fsub";
        case IR_OP_FMUL: return "fmul";
        case IR_OP_FDIV: return "fdiv";
        case IR_OP_SHL: return "shl";
        case IR_OP_LSHR: return "lshr";
        case IR_OP_ASHR: return "ashr";
        case IR_OP_AND: return "and";
        case IR_OP_OR: return "or";
        case IR_OP_XOR: return "xor";
        case IR_OP_ALLOCA: return "alloca";
        case IR_OP_LOAD: return "load";
        case IR_OP_STORE: return "store";
        case IR_OP_GETELEMENTPTR: return "getelementptr";
        case IR_OP_ICMP: return "icmp";
        case IR_OP_FCMP: return "fcmp";
        case IR_OP_PHI: return "phi";
        case IR_OP_CALL: return "call";
        case IR_OP_SITOFP: return "sitofp";
        case IR_OP_FPTOSI: return "fptosi";
        case IR_OP_ZEXT: return "zext";
        case IR_OP_FPEXT: return "fpext";
        case IR_OP_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

/**
 * @brief 将内部的 Type 结构打印为类 LLVM IR 的文本格式。
 * @param type 要打印的类型。
 * @param out 目标输出流。
 */
static void print_type(Type* type, FILE* out) {
    if (!type) {
        fprintf(out, "void");
        return;
    }
    
    switch (type->kind) {
        case TYPE_BASIC:
            switch (type->basic) {
                case BASIC_INT: fprintf(out, "i32"); break;
                case BASIC_FLOAT: fprintf(out, "float"); break;
                case BASIC_I1: fprintf(out, "i1"); break;
                case BASIC_I8: fprintf(out, "i8"); break;
                case BASIC_I64: fprintf(out, "i64"); break;
                case BASIC_DOUBLE: fprintf(out, "double"); break;
            }
            break;
        case TYPE_ARRAY: {
            // LLVM IR风格：递归嵌套 [N x ...] 最内层是元素类型
            // 先递归到最内层
            const Type* t = type;
            size_t dim_stack[16]; // 假设不会超过16维
            size_t dim_count = 0;
            while (t && t->kind == TYPE_ARRAY) {
                if (dim_count < 16) {
                    if (t->array.dim_count == 1 && !t->array.dimensions[0].is_dynamic) {
                        dim_stack[dim_count++] = t->array.dimensions[0].static_size;
                    } else if (t->array.dim_count == 1 && t->array.dimensions[0].is_dynamic) {
                        dim_stack[dim_count++] = (size_t)-1; // 用-1表示动态
                    } else {
                        // 多维数组，逐层展开
                        for (size_t i = 0; i < t->array.dim_count; ++i) {
                            if (!t->array.dimensions[i].is_dynamic)
                                dim_stack[dim_count++] = t->array.dimensions[i].static_size;
                            else
                                dim_stack[dim_count++] = (size_t)-1;
                        }
                    }
                }
                t = t->array.element_type;
            }
            // 先打印所有外层维度
            for (size_t i = 0; i < dim_count; ++i) {
                if (dim_stack[i] == (size_t)-1)
                    fprintf(out, "[? x ");
                else
                    fprintf(out, "[%zu x ", dim_stack[i]);
            }
            // 打印最内层元素类型
            print_type((Type*)t, out);
            // 关闭所有括号
            for (size_t i = 0; i < dim_count; ++i) fprintf(out, "]");
            break;
        }
        case TYPE_FUNCTION:
            print_type(type->function.return_type, out);
            fprintf(out, "(");
            for (size_t i = 0; i < type->function.param_count; i++) {
                if (i > 0) fprintf(out, ", ");
                print_type(type->function.param_types[i], out);
            }
            if (type->function.is_variadic) {
                fprintf(out, ", ...");
            }
            fprintf(out, ")");
            break;
        default:
            fprintf(out, "<unsupported type>");
            break;
    }
}

// 前向声明，供 print_constant_aggregate 调用
static void print_value(IRValue* value, FILE* out);

/**
 * @brief 递归打印聚合常量
 * @param agg 要打印的聚合常量。
 * @param out 目标输出流。
 */
static void print_constant_aggregate(ConstantAggregate* agg, FILE* out) {
    fprintf(out, "[");
    for (size_t i = 0; i < agg->count; ++i) {
        if (i > 0) fprintf(out, ", ");
        print_value(agg->elements[i], out);
    }
    fprintf(out, "]");
}

/**
 * @brief 将一个 IRValue 打印为文本格式。
 * @details 常量直接打印其值，非常量（寄存器、全局变量等）打印其名称。
 * @param value 要打印的值。
 * @param out 目标输出流。
 */
static void print_value(IRValue* value, FILE* out) {
    if (!value) {
        fprintf(out, "null");
        return;
    }
    
    if (value->is_constant) {
        if (value->type && value->type->kind == TYPE_BASIC) {
            switch (value->type->basic) {
                case BASIC_INT:
                    fprintf(out, "%d", value->int_val);
                    break;
                case BASIC_FLOAT:
                    fprintf(out, "%f", value->float_val);
                    break;
                case BASIC_I1:
                    fprintf(out, "%d", value->int_val ? 1 : 0);
                    break;
                case BASIC_I8:
                    fprintf(out, "%d", value->int_val);
                    break;
                case BASIC_I64:
                    fprintf(out, "%ld", value->i64_val);
                    break;
                case BASIC_DOUBLE:
                    fprintf(out, "%f", value->double_val);
                    break;
            }
        } else if (value->type && value->type->kind == TYPE_ARRAY) {
            print_constant_aggregate(&value->aggregate, out);
        } else {
            fprintf(out, "constant");
        }
    } else {
        // 非常量（寄存器、函数、全局变量等）打印其名称，区分全局/局部
        if (value->is_global || (value->type && value->type->kind == TYPE_FUNCTION)) {
            fprintf(out, "@%s", value->name);
        } else {
            fprintf(out, "%%%s", value->name);
        }
    }
}

/**
 * @brief 将单条 IR 指令打印为文本格式。
 * @param instr 要打印的指令。
 * @param out 目标输出流。
 */
static void print_instruction(IRInstruction* instr, FILE* out) {
    if (!instr) return;
    
    // PHI 节点特殊打印
    if (instr->opcode == IR_OP_PHI) {
        if (instr->dest) {
            print_value(instr->dest, out);
            fprintf(out, " = ");
        }
        fprintf(out, "phi ");
        print_type(instr->dest ? instr->dest->type : NULL, out);
        IROperand* op = instr->operand_head;
        int first = 1;
        while (op && op->next_in_instr) {
            if (!first) fprintf(out, ",");
            fprintf(out, " [");
            print_value(op->data.value, out);
            fprintf(out, ", %%");
            IROperand* pred_op = op->next_in_instr;
            fprintf(out, "%s", pred_op->data.bb->label);
            fprintf(out, "]");
            op = pred_op->next_in_instr;
            first = 0;
        }
        fprintf(out, "\n");
        return;
    }
    
    // 如果指令有返回值，先打印目标寄存器。
    if (instr->dest) {
        print_value(instr->dest, out);
        fprintf(out, " = ");
    }
    
    // 打印操作码。
    fprintf(out, "%s", opcode_to_string(instr->opcode));
    
    // 如果是比较指令，打印条件码。
    if (instr->opcode == IR_OP_ICMP || instr->opcode == IR_OP_FCMP) {
        if (instr->opcode_cond) {
            fprintf(out, " %s", instr->opcode_cond);
        }
    }
    
    // 遍历并打印所有操作数。
    IROperand* op = instr->operand_head;
    while (op) {
        fprintf(out, " ");
        if (op->kind == IR_OP_KIND_VALUE) {
            print_value(op->data.value, out);
        } else {
            fprintf(out, "label %%%s", op->data.bb->label);
        }
        op = op->next_in_instr;
    }
    
    fprintf(out, "\n");
}

/**
 * @brief 将单个基本块及其所有指令打印为文本格式。
 * @param bb 要打印的基本块。
 * @param out 目标输出流。
 */
void print_basic_block(IRBasicBlock* bb, FILE* out) {
    if (!bb) return;
    
    // 打印基本块的标签。
    fprintf(out, "%s:\n", bb->label);
    
    // 遍历并打印块内的每一条指令。
    IRInstruction* instr = bb->head;
    while (instr) {
        fprintf(out, "  "); // 缩进
        print_instruction(instr, out);
        instr = instr->next;
    }
    fprintf(out, "\n");
}

/**
 * @brief 将单个函数及其所有基本块打印为文本格式。
 * @param func 要打印的函数。
 * @param out 目标输出流。
 */
void print_function(IRFunction* func, FILE* out) {
    if (!func) return;
    fprintf(out, "define ");
    print_type(func->return_type, out);
    fprintf(out, " @%s(", func->name);
    for (int i = 0; i < func->num_args; ++i) {
        if (i > 0) fprintf(out, ", ");
        IRValue* arg = func->args[i];
        print_type(arg->type, out);
        fprintf(out, " %%");
        fprintf(out, "%s", arg->name ? arg->name : "arg");
    }
    fprintf(out, ") {\n");
    IRBasicBlock* bb = func->blocks;
    while (bb) {
        print_basic_block(bb, out);
        bb = bb->next_in_func;
    }
    fprintf(out, "}\n\n");
}

/**
 * @brief 将整个 IR 模块打印为文本格式。
 * @param module 要打印的模块。
 * @param out 目标输出流。
 */
void print_module(IRModule* module, FILE* out) {
    if (!module) return;
    
    // 打印模块元信息。
    fprintf(out, "; ModuleID = '%s'\n", module->source_filename);
    fprintf(out, "source_filename = \"%s\"\n\n", module->source_filename);
    
    // 打印所有全局变量。
    IRGlobalVariable* global = module->globals;
    while (global) {
        fprintf(out, "@%s = ", global->name);
        if (global->is_const) {
            fprintf(out, "constant ");
        } else {
            fprintf(out, "global ");
        }
        print_type(global->type, out);
        if (global->initializer) {
            fprintf(out, " ");
            print_value(global->initializer, out);
        } else {
            fprintf(out, " zeroinitializer");
        }
        fprintf(out, "\n");
        global = global->next;
    }
    if (module->globals) fprintf(out, "\n");
    
    // 打印所有函数。
    IRFunction* func = module->functions;
    while (func) {
        print_function(func, out);
        func = func->next;
    }
}

/**
 * @brief 将 IR 模块打印到指定的文件。
 * @param module 要打印的模块。
 * @param filename 目标文件名。
 */
void print_ir_to_file(IRModule* module, const char* filename) {
    FILE* out = fopen(filename, "w");
    if (!out) {
        fprintf(stderr, "Error: Could not open file '%s' for writing\n", filename);
        return;
    }
    
    print_module(module, out);
    fclose(out);
}