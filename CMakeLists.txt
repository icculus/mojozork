project(mojozork)
cmake_minimum_required(VERSION 3.0.0)
add_executable(mojozork mojozork.c)

if(MSVC)
    add_definitions(_CRT_SECURE_NO_WARNINGS)
    add_definitions(_CRT_SECURE_NO_DEPRECATE)
    add_definitions(_CRT_NONSTDC_NO_DEPRECATE)
endif()
