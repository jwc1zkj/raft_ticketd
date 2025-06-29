set(TGT tpl)

if (WIN32)
    set(SRCS
        win/mmap.c 
        win/mman.h 
        win/nonempty.c
    )
endif()

add_library(${TGT} STATIC
    tpl.c
    tpl.h
    ${SRCS}
)

target_include_directories(${TGT} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(${TGT} PUBLIC TPL_NOLIB)

