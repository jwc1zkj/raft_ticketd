set(TGT uv_multiplex)

add_library(${TGT} STATIC 
    dispatcher.c
    uv_multiplex.h
    worker.c
)

target_include_directories(${TGT} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(libuv CONFIG REQUIRED)
target_link_libraries(${TGT} PUBLIC container_of)
target_link_libraries(${TGT} PUBLIC $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>)
