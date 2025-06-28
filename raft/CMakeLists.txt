set(TGT raft)

add_library(${TGT} STATIC
    include/raft_log.h
    include/raft_private.h
    include/raft_types.h
    include/raft.h
    src/raft_log.c 
    src/raft_node.c 
    src/raft_server_properties.c
    src/raft_server.c
)

target_include_directories(${TGT} PUBLIC include)
