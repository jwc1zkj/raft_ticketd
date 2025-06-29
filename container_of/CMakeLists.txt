set(TGT container_of)

add_library(${TGT} INTERFACE 
    container_of.h
)

target_include_directories(${TGT} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
