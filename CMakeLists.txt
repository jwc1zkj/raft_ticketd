cmake_minimum_required(VERSION 3.18)

if (WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "" FORCE)
else()
    set(VCPKG_TARGET_TRIPLET "x86-linux" CACHE STRING "" FORCE)
endif()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}\\scripts\\buildsystems\\vcpkg.cmake")

project(raft_ticketd LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_INSTALL_BINDIR ".")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

message("CMAKE_CURRENT_BINARY_DIR: ${CMAKE_CURRENT_BINARY_DIR}")

set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/bin")

if (MSVC)
    add_compile_options("/utf-8")
    # add_compile_options("/Za")
    add_compile_options("/permissive-")
    add_compile_definitions("NOMINMAX")
else()
    add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG>")
endif()

configure_file("version.h.in" "version.h" @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

include(CTest)

add_subdirectory("arraytools")
add_subdirectory("container_of")
add_subdirectory("uv_helpers")
add_subdirectory("lmdb_helpers")
add_subdirectory("raft")
add_subdirectory("tpl")
add_subdirectory("src")

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
include(InstallRequiredSystemLibraries)
include(CPack)
