project(mojozork)
cmake_minimum_required(VERSION 3.0.0)
add_executable(mojozork mojozork.c)

if(NOT MSVC)  # the daemon is pretty Unixy.
    add_executable(multizorkd multizorkd.c)
endif()

if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
endif()

