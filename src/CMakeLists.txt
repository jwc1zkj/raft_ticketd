set(TGT ticketd)

add_executable(${TGT}
    main.c
)

target_link_libraries(${TGT} 
    arraytools 
    container_of 
    picohttpparser
    lmdb_helpers
    uv_helpers
    uv_multiplex
    raft
    tpl
)
