#ifndef SRC_HTTP_SERVER_H_
#define SRC_HTTP_SERVER_H_
#include <thread>
#include <memory>

#include "main.h"

class http_server_impl;
class http_server
{
    std::thread m_thd;
    std::unique_ptr<http_server_impl> m_sp;
public:
    http_server();
    ~http_server();

    void start(server_t* sv, const char *addr, const char *service, int num_workers, bool spin = false);
};

#endif // SRC_HTTP_SERVER_H_