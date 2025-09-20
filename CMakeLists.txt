cmake_minimum_required(VERSION 3.16)
project(mojozork)

set(MOJOZORK_STANDALONE_DEFAULT ON)
set(MOJOZORK_MULTIZORK_DEFAULT ON)
set(MOJOZORK_LIBRETRO_DEFAULT ON)
set(MOJOZORK_SDL3_DEFAULT ON)

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
    set(MOJOZORK_SDL3_DEFAULT OFF)
endif()

option(MOJOZORK_STANDALONE "Build the MojoZork standalone app" ${MOJOZORK_STANDALONE_DEFAULT})
option(MOJOZORK_MULTIZORK "Build the Multizork server" ${MOJOZORK_MULTIZORK_DEFAULT})
option(MOJOZORK_LIBRETRO "Build the MojoZork libretro core" ${MOJOZORK_LIBRETRO_DEFAULT})
option(MOJOZORK_SDL3 "Build the MojoZork standalone SDL3 app" ${MOJOZORK_SDL3_DEFAULT})

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

if(MOJOZORK_SDL3)
    find_package(SDL3)
    if (NOT SDL3_FOUND)
        # FetchContent downloads and configures dependencies
        include(FetchContent)
        FetchContent_Declare(
            SDL3
            GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
            GIT_TAG "main"
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(SDL3)
    endif()

    add_executable(mojozork_sdl3 mojozork-sdl3.c)

    target_link_libraries(mojozork_sdl3 SDL3::SDL3)
    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        set_target_properties(mojozork_sdl3 PROPERTIES OUTPUT_NAME "mojozork-wasm")
        set_target_properties(mojozork_sdl3 PROPERTIES LINK_FLAGS "-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s ASSERTIONS=0")
    endif()
endif()

# end of CMakeLists.txt ...

