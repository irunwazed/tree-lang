cmake_minimum_required(VERSION 3.20)
project(forest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FOREST_ASAN "Build the compiler with AddressSanitizer" OFF)

add_executable(forest
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
    target_compile_options(forest PRIVATE /W4 /utf-8 /permissive- /wd4244 /wd4267)
    target_compile_definitions(forest PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(FOREST_ASAN)
        target_compile_options(forest PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(forest PRIVATE -Wall -Wextra)
    if(FOREST_ASAN)
        target_compile_options(forest PRIVATE -fsanitize=address)
        target_link_options(forest PRIVATE -fsanitize=address)
    endif()
endif()

# Salin runtime dan library standar di samping executable agar compiler bisa menemukannya.
add_custom_command(TARGET forest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/runtime $<TARGET_FILE_DIR:forest>/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/stdlib $<TARGET_FILE_DIR:forest>/stdlib)
