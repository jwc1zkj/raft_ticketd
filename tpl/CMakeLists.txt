set(TGT tpl)

add_library(${TGT}
    tpl.c
    tpl.h
)

if (WIN32)
    target_sources(${TGT} PUBLIC
        win/mmap.c 
        win/mman.h 
        win/nonempty.c
    )
endif()

target_include_directories(${TGT} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(${TGT} PRIVATE TPL_NOLIB)

