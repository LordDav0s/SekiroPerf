cmake_minimum_required(VERSION 3.15)
project(SekiroPerf CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Sekiro is 64-bit: configure for x64 (e.g. cmake -A x64).")
    endif()

    file(GLOB SEKIRO_PERF_SOURCES CONFIGURE_DEPENDS src/*.cpp)
    add_library(sekiro_perf SHARED ${SEKIRO_PERF_SOURCES} src/exports.def)
    set_target_properties(sekiro_perf PROPERTIES PREFIX "")  # MinGW would otherwise name it libsekiro_perf.dll
    target_compile_definitions(sekiro_perf PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601 WINVER=0x0601 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(sekiro_perf PRIVATE user32)

    if(MSVC)
        set_property(TARGET sekiro_perf PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # no VC++ redist needed
        target_compile_options(sekiro_perf PRIVATE /W4 /permissive-)
    else()
        target_compile_options(sekiro_perf PRIVATE -Wall -Wextra)
        target_link_options(sekiro_perf PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

# Portable unit tests (signatures, FSR constants); build with the host compiler.
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(sekiro_perf_tests tests/test_main.cpp tests/test_sigs.cpp tests/test_fsr.cpp)
    target_include_directories(sekiro_perf_tests PRIVATE src)
    add_test(NAME sekiro_perf_tests COMMAND sekiro_perf_tests)
endif()
