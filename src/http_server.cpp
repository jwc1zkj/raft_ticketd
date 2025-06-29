//
// Copyright (c) 2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: HTTP server, fast
//
//------------------------------------------------------------------------------
#include "http_server.h"
#include "fields_alloc.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <string>

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

// Return a reasonable mime type based on the extension of a file.
beast::string_view
mime_type(beast::string_view path)
{
    using beast::iequals;
    auto const ext = [&path]
    {
        auto const pos = path.rfind(".");
        if (pos == beast::string_view::npos)
            return beast::string_view{};
        return path.substr(pos);
    }();
    if (iequals(ext, ".htm"))
        return "text/html";
    if (iequals(ext, ".html"))
        return "text/html";
    if (iequals(ext, ".php"))
        return "text/html";
    if (iequals(ext, ".css"))
        return "text/css";
    if (iequals(ext, ".txt"))
        return "text/plain";
    if (iequals(ext, ".js"))
        return "application/javascript";
    if (iequals(ext, ".json"))
        return "application/json";
    if (iequals(ext, ".xml"))
        return "application/xml";
    if (iequals(ext, ".swf"))
        return "application/x-shockwave-flash";
    if (iequals(ext, ".flv"))
        return "video/x-flv";
    if (iequals(ext, ".png"))
        return "image/png";
    if (iequals(ext, ".jpe"))
        return "image/jpeg";
    if (iequals(ext, ".jpeg"))
        return "image/jpeg";
    if (iequals(ext, ".jpg"))
        return "image/jpeg";
    if (iequals(ext, ".gif"))
        return "image/gif";
    if (iequals(ext, ".bmp"))
        return "image/bmp";
    if (iequals(ext, ".ico"))
        return "image/vnd.microsoft.icon";
    if (iequals(ext, ".tiff"))
        return "image/tiff";
    if (iequals(ext, ".tif"))
        return "image/tiff";
    if (iequals(ext, ".svg"))
        return "image/svg+xml";
    if (iequals(ext, ".svgz"))
        return "image/svg+xml";
    return "application/text";
}

class http_worker
{
public:
    http_worker(http_worker const &) = delete;
    http_worker &operator=(http_worker const &) = delete;

    http_worker(server_t *sv, tcp::acceptor &acceptor, const std::string &doc_root) : sv_{sv}, acceptor_(acceptor),
                                                                                      doc_root_(doc_root)
    {
    }

    void start()
    {
        accept();
        check_deadline();
    }

private:
    using alloc_t = fields_alloc<char>;
    // using request_body_t = http::basic_dynamic_body<beast::flat_static_buffer<1024 * 1024>>;
    using request_body_t = http::string_body;

    server_t *sv_;

    // The acceptor used to listen for incoming connections.
    tcp::acceptor &acceptor_;

    // The path to the root of the document directory.
    std::string doc_root_;

    // The socket for the currently connected client.
    tcp::socket socket_{acceptor_.get_executor()};

    // The buffer for performing reads
    beast::flat_static_buffer<8192> buffer_;

    // The allocator used for the fields in the request and reply.
    alloc_t alloc_{8192};

    // The parser for reading the requests
    boost::optional<http::request_parser<request_body_t, alloc_t>> parser_;

    // The timer putting a time limit on requests.
    net::steady_timer request_deadline_{
        acceptor_.get_executor(), (std::chrono::steady_clock::time_point::max)()};

    // The string-based response message.
    boost::optional<http::response<http::string_body, http::basic_fields<alloc_t>>> string_response_;

    // The string-based response serializer.
    boost::optional<http::response_serializer<http::string_body, http::basic_fields<alloc_t>>> string_serializer_;

    // The file-based response message.
    boost::optional<http::response<http::file_body, http::basic_fields<alloc_t>>> file_response_;

    // The file-based response serializer.
    boost::optional<http::response_serializer<http::file_body, http::basic_fields<alloc_t>>> file_serializer_;

    void accept()
    {
        // Clean up any previous connection.
        beast::error_code ec;
        socket_.close(ec);
        buffer_.consume(buffer_.size());

        acceptor_.async_accept(
            socket_,
            [this](beast::error_code ec)
            {
                if (ec)
                {
                    accept();
                }
                else
                {
                    // Request must be fully processed within 60 seconds.
                    request_deadline_.expires_after(
                        std::chrono::seconds(60));

                    read_request();
                }
            });
    }

    void read_request()
    {
        // On each read the parser needs to be destroyed and
        // recreated. We store it in a boost::optional to
        // achieve that.
        //
        // Arguments passed to the parser constructor are
        // forwarded to the message object. A single argument
        // is forwarded to the body constructor.
        //
        // We construct the dynamic body with a 1MB limit
        // to prevent vulnerability to buffer attacks.
        //
        parser_.emplace(
            std::piecewise_construct,
            std::make_tuple(),
            std::make_tuple(alloc_));

        http::async_read(
            socket_,
            buffer_,
            *parser_,
            [this](beast::error_code ec, std::size_t)
            {
                if (ec)
                    accept();
                else
                    process_request(parser_->get());
            });
    }

    void process_request(http::request<request_body_t, http::basic_fields<alloc_t>> const &req)
    {
        switch (req.method())
        {
        case http::verb::get:
            send_file(req.target());
            break;
        case http::verb::post:
            http_get_id();
            break;
        default:
            // We return responses indicating an error if
            // we do not recognize the request method.
            send_bad_response(
                http::status::bad_request,
                "Invalid request-method '" + std::string(req.method_string()) + "'\r\n");
            break;
        }
    }

    void send_bad_response(
        http::status status,
        std::string const &error)
    {
        string_response_.emplace(
            std::piecewise_construct,
            std::make_tuple(),
            std::make_tuple(alloc_));

        string_response_->result(status);
        string_response_->keep_alive(false);
        string_response_->set(http::field::server, "Beast");
        string_response_->set(http::field::content_type, "text/plain");
        string_response_->body() = error;
        string_response_->prepare_payload();

        string_serializer_.emplace(*string_response_);

        http::async_write(
            socket_,
            *string_serializer_,
            [this](beast::error_code ec, std::size_t)
            {
                socket_.shutdown(tcp::socket::shutdown_send, ec);
                string_serializer_.reset();
                string_response_.reset();
                accept();
            });
    }

    void send_file(beast::string_view target)
    {
        // Request path must be absolute and not contain "..".
        if (target.empty() || target[0] != '/' || target.find("..") != std::string::npos)
        {
            send_bad_response(
                http::status::not_found,
                "File not found\r\n");
            return;
        }

        std::string full_path = doc_root_;
        full_path.append(
            target.data(),
            target.size());

        http::file_body::value_type file;
        beast::error_code ec;
        file.open(
            full_path.c_str(),
            beast::file_mode::read,
            ec);
        if (ec)
        {
            send_bad_response(
                http::status::not_found,
                "File not found\r\n");
            return;
        }

        file_response_.emplace(
            std::piecewise_construct,
            std::make_tuple(),
            std::make_tuple(alloc_));

        file_response_->result(http::status::ok);
        file_response_->keep_alive(false);
        file_response_->set(http::field::server, "Beast");
        file_response_->set(http::field::content_type, mime_type(std::string(target)));
        file_response_->body() = std::move(file);
        file_response_->prepare_payload();

        file_serializer_.emplace(*file_response_);

        http::async_write(
            socket_,
            *file_serializer_,
            [this](beast::error_code ec, std::size_t)
            {
                socket_.shutdown(tcp::socket::shutdown_send, ec);
                file_serializer_.reset();
                file_response_.reset();
                accept();
            });
    }

    void http_get_id()
    {
        raft_node_t *leader = raft_get_current_leader_node(sv_->raft);
        if (!leader)
            return send_bad_response(http::status::service_unavailable /* 503 */, "Leader unavailable");
        else if (raft_node_get_id(leader) != sv_->node_id)
        {
            peer_connection_t *leader_conn = static_cast<peer_connection_t *>(raft_node_get_udata(leader));

            char leader_url[LEADER_URL_LEN];
            snprintf(leader_url, LEADER_URL_LEN, "http://%s:%d/",
                     inet_ntoa(leader_conn->addr.sin_addr),
                     leader_conn->http_port);

            string_response_.emplace(
                std::piecewise_construct,
                std::make_tuple(),
                std::make_tuple(alloc_));

            string_response_->result(http::status::moved_permanently);
            string_response_->keep_alive(false);
            string_response_->set(http::field::server, "Beast");
            string_response_->set(http::field::content_type, "text/plain");
            string_response_->set(http::field::location, leader_url);
            // string_response_->body() = error;
            // string_response_->prepare_payload();

            string_serializer_.emplace(*string_response_);

            http::async_write(
                socket_,
                *string_serializer_,
                [this](beast::error_code ec, std::size_t)
                {
                    socket_.shutdown(tcp::socket::shutdown_send, ec);
                    string_serializer_.reset();
                    string_response_.reset();
                    accept();
                });
            return;
        }

        int e;

        unsigned int ticket = __generate_ticket();

        msg_entry_t entry = {};
        entry.id = rand();
        entry.data.buf = (void *)&ticket;
        entry.data.len = sizeof(ticket);

        uv_mutex_lock(&sv_->raft_lock);

        msg_entry_response_t r;
        e = raft_recv_entry(sv_->raft, &entry, &r);
        if (0 != e)
            return send_bad_response(http::status::internal_server_error /* 500 */, "BAD");

        /* block until the entry is committed */
        int done = 0, tries = 0;
        do
        {
            if (3 < tries)
            {
                printf("ERROR: failed to commit entry\n");
                uv_mutex_unlock(&sv_->raft_lock);
                return send_bad_response(http::status::bad_request /* 400 */, "TRY AGAIN");
            }

            uv_cond_wait(&sv_->appendentries_received, &sv_->raft_lock);
            e = raft_msg_entry_response_committed(sv_->raft, &r);
            tries += 1;
            switch (e)
            {
            case 0:
                /* not committed yet */
                break;
            case 1:
                done = 1;
                uv_mutex_unlock(&sv_->raft_lock);
                break;
            case -1:
                uv_mutex_unlock(&sv_->raft_lock);
                return send_bad_response(http::status::bad_request /* 400 */, "TRY AGAIN");
            }
        } while (!done);

        /* serialize ID */
        string_response_.emplace(
            std::piecewise_construct,
            std::make_tuple(),
            std::make_tuple(alloc_));

        string_response_->result(http::status::ok);
        string_response_->keep_alive(false);
        string_response_->set(http::field::server, "Beast");
        string_response_->set(http::field::content_type, "text/plain");

        char id_str[100];
        sprintf(id_str, "%d", entry.id);
        string_response_->body() = id_str;
        string_response_->prepare_payload();

        string_serializer_.emplace(*string_response_);

        http::async_write(
            socket_,
            *string_serializer_,
            [this](beast::error_code ec, std::size_t)
            {
                socket_.shutdown(tcp::socket::shutdown_send, ec);
                string_serializer_.reset();
                string_response_.reset();
                accept();
            });
        return;
    }

    void check_deadline()
    {
        // The deadline may have moved, so check it has really passed.
        if (request_deadline_.expiry() <= std::chrono::steady_clock::now())
        {
            // Close socket to cancel any outstanding operation.
            socket_.close();

            // Sleep indefinitely until we're given a new deadline.
            request_deadline_.expires_at(
                (std::chrono::steady_clock::time_point::max)());
        }

        request_deadline_.async_wait(
            [this](beast::error_code)
            {
                check_deadline();
            });
    }
};

class http_server_impl
{
    net::io_context ioc_{1};
    server_t *sv_;
    net::ip::address address_;
    unsigned short port_;
    tcp::acceptor acceptor_;
    std::list<http_worker> workers_;
    std::string doc_root_;
    int num_workers_;
    bool spin_;

public:
    http_server_impl(server_t *sv, const char *addr, const char *service, int num_workers, bool spin = false)
        : sv_{sv}, address_(net::ip::make_address(addr)), port_(static_cast<unsigned short>(std::atoi(service))), acceptor_{ioc_, {address_, port_}}, num_workers_{num_workers}, spin_{spin}
    {
    }
    ~http_server_impl()
    {
    }

    void run()
    {
        for (int i = 0; i < num_workers_; ++i)
        {
            workers_.emplace_back(sv_, acceptor_, "/");
            workers_.back().start();
        }

        if (spin_)
            for (;;)
                ioc_.poll();
        else
            ioc_.run();
    }
};

http_server::http_server()
{
}

http_server::~http_server()
{
}

void http_server::start(server_t *sv, const char *addr, const char *service, int num_workers, bool spin)
{
    m_sp = std::make_unique<http_server_impl>(sv, addr, service, num_workers, spin);
    m_thd = std::move(std::thread([&]
                                  { m_sp->run(); }));
}

#if 0
int main(int argc, char *argv[])
{
    try
    {
        // Check command line arguments.
        if (argc != 6)
        {
            std::cerr << "Usage: http_server_fast <address> <port> <doc_root> <num_workers> {spin|block}\n";
            std::cerr << "  For IPv4, try:\n";
            std::cerr << "    http_server_fast 0.0.0.0 80 . 100 block\n";
            std::cerr << "  For IPv6, try:\n";
            std::cerr << "    http_server_fast 0::0 80 . 100 block\n";
            return EXIT_FAILURE;
        }

        auto const address = net::ip::make_address(argv[1]);
        unsigned short port = static_cast<unsigned short>(std::atoi(argv[2]));
        std::string doc_root = argv[3];
        int num_workers = std::atoi(argv[4]);
        bool spin = (std::strcmp(argv[5], "spin") == 0);

        net::io_context ioc{1};
        tcp::acceptor acceptor{ioc, {address, port}};

        std::list<http_worker> workers;
        for (int i = 0; i < num_workers; ++i)
        {
            workers.emplace_back(acceptor, doc_root);
            workers.back().start();
        }

        if (spin)
            for (;;)
                ioc.poll();
        else
            ioc.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
#endif
