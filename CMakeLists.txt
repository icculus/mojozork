cmake_minimum_required(VERSION 3.5.0)
project(mojozork)

set(MOJOZORK_STANDALONE_DEFAULT ON)
set(MOJOZORK_MULTIZORK_DEFAULT ON)
set(MOJOZORK_LIBRETRO_DEFAULT ON)

if(MSVC)   # the daemon is pretty Unixy.
    set(MOJOZORK_MULTIZORK_DEFAULT OFF)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(ANDROID TRUE)
endif()

# Building as part of RetroArch? Turn off everything but the libretro plugin by default.
if(LIBRETRO)
    set(MOJOZORK_STANDALONE_DEFAULT OFF)
    set(MOJOZORK_MULTIZORK_DEFAULT OFF)
endif()

option(MOJOZORK_STANDALONE "Build the MojoZork standalone app" ${MOJOZORK_STANDALONE_DEFAULT})
option(MOJOZORK_MULTIZORK "Build the Multizork server" ${MOJOZORK_MULTIZORK_DEFAULT})
option(MOJOZORK_LIBRETRO "Build the MojoZork libretro core" ${MOJOZORK_LIBRETRO_DEFAULT})

if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
else()  # This assumes you have GCC or Clang at the moment.
    add_definitions(-Wall)
endif()

if(MOJOZORK_STANDALONE)
    add_executable(mojozork mojozork.c)
endif()

if(MOJOZORK_MULTIZORK)
    add_executable(multizorkd multizorkd.c)
    target_link_libraries(multizorkd -lsqlite3)
endif()

if(MOJOZORK_LIBRETRO)
    add_library(mojozork_libretro SHARED mojozork-libretro.c)
    if(ANDROID)  # Android builds of libretro cores need specific names.
        set_target_properties(mojozork_libretro PROPERTIES LIBRARY_OUTPUT_NAME mojozork_libretro_android)
    endif()
    set_target_properties(mojozork_libretro PROPERTIES PREFIX "")
endif()

# end of CMakeLists.txt ...

