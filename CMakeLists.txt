# ==============================================================================
# 1. Project Basic Configuration
# ==============================================================================
cmake_minimum_required(VERSION 3.16)

# Specify Clang as the compiler
find_program(CLANG_EXECUTABLE clang)
find_program(CLANG_PLUS_PLUS_EXECUTABLE clang++)

if(CLANG_EXECUTABLE)
    set(CMAKE_C_COMPILER ${CLANG_EXECUTABLE})
endif()

if(CLANG_PLUS_PLUS_EXECUTABLE)
    set(CMAKE_CXX_COMPILER ${CLANG_PLUS_PLUS_EXECUTABLE})
endif()
project(SysYCompiler C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ==============================================================================
# 1.5. IWYU Integration (Optional)
# ==============================================================================

#find_program(IWYU_EXECUTABLE include-what-you-use)
#if(IWYU_EXECUTABLE)
#    message(STATUS "Found include-what-you-use, enabling for C files: ${IWYU_EXECUTABLE}")
    # Tell CMake to run IWYU alongside the C compiler
#    set(CMAKE_C_INCLUDE_WHAT_YOU_USE ${IWYU_EXECUTABLE})
#else()
#    message(WARNING "include-what-you-use not found. Build will continue without it.")
#endif()

# ==============================================================================
# 2. Find Dependencies (Flex & Bison only)
# ==============================================================================
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

# ==============================================================================
# 3. Code Generation (Flex & Bison)
# ==============================================================================
set(GENERATED_SOURCES_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_SOURCES_DIR})

set(BISON_OUTPUT_C ${GENERATED_SOURCES_DIR}/parser.tab.c)
set(BISON_OUTPUT_H ${GENERATED_SOURCES_DIR}/parser.tab.h)
set(FLEX_OUTPUT_C ${GENERATED_SOURCES_DIR}/lexer.yy.c)

BISON_TARGET(SysYParser ${CMAKE_SOURCE_DIR}/src/parser/parser.y ${BISON_OUTPUT_C} DEFINES_FILE ${BISON_OUTPUT_H})
FLEX_TARGET(SysYLexer ${CMAKE_SOURCE_DIR}/src/lexer/lexer.l ${FLEX_OUTPUT_C})

set_source_files_properties(${FLEX_OUTPUT_C} PROPERTIES OBJECT_DEPENDS ${BISON_OUTPUT_H})
set_source_files_properties(${BISON_OUTPUT_C}
    PROPERTIES COMPILE_FLAGS "-Wno-unused-but-set-variable"
)

# ==============================================================================
# 4. Define Source Files
# ==============================================================================
set(COMPILER_SOURCES
    # AST and core components
    src/ast/ast.c
    
    # Parser and lexer (generated)
    ${BISON_OUTPUT_C}
    ${FLEX_OUTPUT_C}
    
    # Semantic analysis
    src/semantic_analyzer/semantic_analyzer.c
    src/symbol_table/symbol_table.c
    
    # IR components
    src/ir/ir_data_structures.c
    src/ir/ir_generator.c
    src/ir/ir_optimizer.c
    src/ir/ir_utils.c
    src/ir/ir_lifecycle.c
    src/ir/ir_builder.c
    src/ir/ir_printer.c
    src/ir/ir_unified.c
    
    # IR analysis passes
    src/ir/analysis/cfg_builder.c
    src/ir/analysis/dominators.c
    src/ir/analysis/loop_analysis.c
    
    # IR transformation passes
    src/ir/transforms/mem2reg.c
    src/ir/transforms/adce.c
    src/ir/transforms/cse.c
    src/ir/transforms/inst_combine.c
    src/ir/transforms/licm.c
    src/ir/transforms/loop_unroll.c
    src/ir/transforms/sccp.c
    src/ir/transforms/simplify_cfg.c
    src/ir/transforms/sroa.c
    src/ir/transforms/ind_var_simplify.c
    src/ir/transforms/tail_call_elim.c
    src/ir/transforms/inliner.c
    
    # Backend
    src/backend/backend_riscv.c
    
    # Driver
    src/driver/driver.c
    
    # Scanner context
    src/scanner/scanner_context.c
    
    # Utilities
    src/utils/error.c
    src/utils/logger.c
)

set(RUNTIME_SOURCES
    runtime/sylib.c
)

# ==============================================================================
# 5. Build Targets
# ==============================================================================
add_library(sylib STATIC ${RUNTIME_SOURCES})
set_target_properties(sylib PROPERTIES OUTPUT_NAME "sylib")

add_executable(sysyc ${COMPILER_SOURCES})

# Modern CMake: Use target_* commands for better control
target_include_directories(sysyc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GENERATED_SOURCES_DIR}
)

# Add POSIX macro definition for all targets
target_compile_definitions(sysyc PRIVATE _POSIX_C_SOURCE=200809L)

# Basic compiler options
target_compile_options(sysyc PRIVATE -g -Wall -Wextra)

# No LLVM libs to link, just Flex and standard math lib
target_link_libraries(sysyc
    PRIVATE
    ${FLEX_LIBRARIES}
    m
)

# ==============================================================================
# 6. Build Configuration
# ==============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(sysyc PRIVATE -O0)
    target_compile_definitions(sysyc PRIVATE DEBUG)
    message(STATUS "Building in Debug mode")
else()
    target_compile_options(sysyc PRIVATE -O2)
    target_compile_definitions(sysyc PRIVATE NDEBUG)
    message(STATUS "Building in Release mode")
endif()

message(STATUS "Compiler executable: sysyc")
message(STATUS "Runtime library: libsylib.a")
message(STATUS "Generated sources directory: ${GENERATED_SOURCES_DIR}")