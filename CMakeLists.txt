cmake_minimum_required(VERSION 3.24)
project(bsxbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(FATAL_ERROR "BSXbox is only supported on Windows")
endif()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BSXbox must be built for x64")
endif()

set(BSX_TLS_PROVIDER_COUNT 8)
set(BSX_TLS_PROVIDER_SIZE 32768)
set(BSX_TLS_PROVIDERS)
math(EXPR BSX_TLS_LAST_PROVIDER "${BSX_TLS_PROVIDER_COUNT} - 1")

foreach(index RANGE 0 ${BSX_TLS_LAST_PROVIDER})
    set(target "bsxtls${index}")
    add_library(${target} SHARED src/tls/tls.cpp)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${target}
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX BSX_TLS_SIZE=${BSX_TLS_PROVIDER_SIZE})
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /GS-)
    list(APPEND BSX_TLS_PROVIDERS ${target})
endforeach()

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER SRC EXCLUDE REGEX "src/tls/")

add_executable(bsxbox ${SRC})
add_dependencies(bsxbox ${BSX_TLS_PROVIDERS})
target_include_directories(bsxbox PRIVATE src/include src/shims)
target_compile_definitions(bsxbox PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(bsxbox PRIVATE /W4 /permissive- /utf-8)

target_link_options(bsxbox PRIVATE /OPT:NOREF)
target_link_libraries(bsxbox PRIVATE psapi ole32 runtimeobject d3d11)