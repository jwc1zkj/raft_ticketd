set(TGT ticketd)

find_package(fmt CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS beast)

add_executable(${TGT}
    main.cpp
)

target_link_libraries(${TGT}
    raft
    tpl
    arraytools 
    container_of 
    lmdb_helpers
    uv_helpers
    uv_multiplex
    fmt::fmt-header-only
    Boost::beast
    Boost::asio
)
