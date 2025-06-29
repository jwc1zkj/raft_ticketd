set(TGT uv_helpers)

add_library(${TGT} STATIC 
    uv_helpers.c
    uv_helpers.h
)

target_include_directories(${TGT} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(libuv CONFIG REQUIRED)
target_link_libraries(${TGT} PUBLIC $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>)
