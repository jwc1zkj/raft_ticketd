set(TGT lmdb_helpers)

add_library(${TGT} STATIC 
    lmdb_helpers.c
    lmdb_helpers.h
)

target_include_directories(${TGT} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(unofficial-lmdb CONFIG REQUIRED)
target_link_libraries(${TGT} PUBLIC unofficial::lmdb::lmdb)
