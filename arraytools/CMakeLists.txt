set(TGT arraytools)

add_library(${TGT} INTERFACE 
    arraytools.h
)

target_include_directories(${TGT} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
