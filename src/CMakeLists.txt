set(TGT ticketd)

find_package(fmt CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS beast)

add_executable(${TGT}
    main.cpp
    fields_alloc.hpp
    http_server.cpp
    http_server.h
)

target_link_libraries(${TGT}
    raft
    tpl
    arraytools 
    container_of 
    lmdb_helpers
    uv_helpers
    fmt::fmt-header-only
    Boost::beast
    Boost::asio
)
