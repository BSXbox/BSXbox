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

add_library(bsxtls SHARED src/tls/tls.cpp)
set_target_properties(bsxtls PROPERTIES
    OUTPUT_NAME bsxtls
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(bsxtls PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(bsxtls PRIVATE /W4 /permissive- /utf-8 /GS-)

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "src/*.cpp")

add_executable(bsxbox ${SRC})
add_dependencies(bsxbox bsxtls)
target_include_directories(bsxbox PRIVATE src/include src/shims)
target_compile_definitions(bsxbox PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(bsxbox PRIVATE /W4 /permissive- /utf-8)

target_link_options(bsxbox PRIVATE /OPT:NOREF)
target_link_libraries(bsxbox PRIVATE psapi ole32 runtimeobject d3d11)