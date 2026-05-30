cmake_minimum_required(VERSION 3.16)
project(lenoc VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(LENO_SOURCES
    src/main.c
    src/error.c
    src/lexer.c
    src/ast.c
    src/scope.c
    src/parser/parser.c
    src/parser/parser_utils.c
    src/parser/parser_expr.c
    src/parser/parser_stmt_control.c
    src/parser/parser_stmt_other.c
    src/parser/parser_func.c
    src/parser/parser_module.c
    src/semantic/semantic.c
    src/semantic/semantic_upvalue.c
    src/semantic/semantic_type.c
    src/semantic/semantic_visit.c
    src/semantic/semantic_visit_method.c
    src/semantic/semantic_visit_ast.c
    src/semantic/semantic_visit_func.c
    src/semantic/semantic_type_utils.c
    src/optimize/optimize.c
    src/gc.c
    src/value.c
    src/string_table.c
    src/object/object_string.c
    src/object/object_array.c
    src/object/object_dict.c
    src/object/object_number.c
    src/object/object_file.c
    src/object/object_struct.c
    src/object/object_face.c
    src/object/object_cstruct.c
    src/object/object_thread.c
    src/bound_method.c
    src/coroutine.c
    src/vm/vm.c
    src/codegen/codegen.c
    src/codegen/codegen_emit.c
    src/codegen/codegen_expr.c
    src/codegen/codegen_stmt.c
    src/codegen/codegen_func.c
    src/codegen/codegen_import.c
    src/codegen/codegen_utils.c
    src/debug.c
    src/type.c
    src/native.c
    src/bigint.c
    src/module_loader.c
    src/module_symbol_table.c
    src/module.c
    src/module/io/io.c
    src/module/types/types.c
    src/module/times/times.c
    src/module/arrays/arrays.c
    src/module/strings/strings.c
    src/module/maths/maths.c
    src/module/dicts/dicts.c
    src/module/structs/structs.c
    src/module/cstructs/cstructs.c
    src/module/rands/rands.c
    src/module/files/files.c
    src/module/asyncs/asyncs.c
    src/module/dirs/dirs.c
    src/module/jsons/jsons.c
    src/module/sockets/sockets.c
    src/module/ffi/ffi.c
    src/module/threads/threads.c
    src/module/assert/assert.c
    src/module/sys/sys.c
    src/module/regexs/regexs.c
    src/module/guis/guis.c
    src/platform/platform_thread.c
    src/serialize/serialize.c
)

if(WIN32)
    list(APPEND LENO_SOURCES src/module/ffi/leno_ffi_win64.c)
    list(APPEND LENO_SOURCES src/module/guis/leno_guis_win32.c)
else()
    list(APPEND LENO_SOURCES src/module/ffi/leno_ffi_linux.c)
    if(APPLE)
        list(APPEND LENO_SOURCES src/module/guis/leno_guis_macos.c)
    else()
        list(APPEND LENO_SOURCES src/module/guis/leno_guis_linux.c)
    endif()
endif()

add_executable(leno ${LENO_SOURCES})

target_include_directories(leno PRIVATE src)

if(MSVC)
    target_compile_options(leno PRIVATE /W4)
else()
    target_compile_options(leno PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(leno PRIVATE ws2_32 gdi32 user32)
    if(MINGW)
        target_link_options(leno PRIVATE -municode)
    endif()
else()
    target_link_libraries(leno PRIVATE m pthread dl)
    if(APPLE)
        find_library(APPKIT_LIBRARY AppKit)
        if(APPKIT_LIBRARY)
            target_link_libraries(leno PRIVATE ${APPKIT_LIBRARY})
        endif()
    else()
        find_library(X11_LIBRARY X11)
        if(X11_LIBRARY)
            target_link_libraries(leno PRIVATE ${X11_LIBRARY})
        endif()
    endif()
    find_library(ICONV_LIBRARY NAMES iconv iconv_hook)
    if(ICONV_LIBRARY)
        target_link_libraries(leno PRIVATE ${ICONV_LIBRARY})
    endif()
endif()

add_executable(test_runner test/test_runner.c)

set(LENO_TEST_DIR "${CMAKE_SOURCE_DIR}/test")

add_custom_target(test
    COMMAND test_runner $<TARGET_FILE:leno> ${LENO_TEST_DIR}
    DEPENDS leno test_runner
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running Leno test suite"
)

install(TARGETS leno RUNTIME DESTINATION bin)
