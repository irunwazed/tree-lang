cmake_minimum_required(VERSION 3.20)
project(treelang CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TREELANG_ASAN "Build the compiler with AddressSanitizer" OFF)

add_executable(treelang
    src/main.cpp
    src/diag.cpp
    src/lexer.cpp
    src/parser.cpp
    src/package.cpp
    src/types.cpp
    src/sema/range.cpp
    src/sema/check.cpp
    src/codegen_llvm.cpp
    src/toolchain.cpp
)

if(MSVC)
    target_compile_options(treelang PRIVATE /W4 /utf-8 /permissive- /wd4244 /wd4267)
    target_compile_definitions(treelang PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(TREELANG_ASAN)
        target_compile_options(treelang PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(treelang PRIVATE -Wall -Wextra)
    if(TREELANG_ASAN)
        target_compile_options(treelang PRIVATE -fsanitize=address)
        target_link_options(treelang PRIVATE -fsanitize=address)
    endif()
endif()

# Salin runtime di samping executable agar compiler bisa menemukannya.
add_custom_command(TARGET treelang POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/runtime $<TARGET_FILE_DIR:treelang>/runtime)
