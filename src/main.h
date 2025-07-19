#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#define NET_LIBRARY_TYPE 0

#include <queue>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <boost/asio.hpp>
#include <boost/pool/pool.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

#include <raft.h>
#include <tpl.h>
#include <lmdb.h>

#define VERSION "0.1.0"
#define ANYPORT 65535
#define MAX_HTTP_CONNECTIONS 128
#define MAX_PEER_CONNECTIONS 128
#define IPV4_STR_LEN 3 * 4 + 3 + 1
#define PERIOD_MSEC 1000
#define RAFT_BUFLEN 512
#define LEADER_URL_LEN 512
#define IPC_PIPE_NAME "ticketd_ipc"
#define HTTP_WORKERS 4
#define IP_STR_LEN sizeof("111.111.111.111")

typedef enum
{
    HANDSHAKE_FAILURE,
    HANDSHAKE_SUCCESS,
} handshake_state_e;

/** Message types used for peer to peer traffic
 * These values are used to identify message types during deserialization */
typedef enum
{
    /** Handshake is a special non-raft message type
     * We send a handshake so that we can identify ourselves to our peers */
    MSG_HANDSHAKE,
    /** Successful responses mean we can start the Raft periodic callback */
    MSG_HANDSHAKE_RESPONSE,
    /** Tell leader we want to leave the cluster */
    /* When instance is ctrl-c'd we have to gracefuly disconnect */
    MSG_DEMOTE,
    MSG_DEMOTE_RESPONSE,
    MSG_LEAVE,
    /* Receiving a leave response means we can shutdown */
    MSG_LEAVE_RESPONSE,
    MSG_REQUESTVOTE,
    MSG_REQUESTVOTE_RESPONSE,
    MSG_APPENDENTRIES,
    MSG_APPENDENTRIES_RESPONSE,
} peer_message_type_e;

/** Peer protocol handshake
 * Send handshake after connecting so that our peer can identify us */
typedef struct
{
    int raft_port;
    int http_port;
    int node_id;
} msg_handshake_t;

typedef struct
{
    int success;

    /* leader's Raft port */
    int leader_port;

    /* the responding node's HTTP port */
    int http_port;

    /* my Raft node ID.
     * Sometimes we don't know who we did the handshake with */
    int node_id;

    char leader_host[IP_STR_LEN];
} msg_handshake_response_t;

/** Add/remove Raft peer */
typedef struct
{
    int raft_port;
    int http_port;
    int node_id;
    char host[IP_STR_LEN];
} entry_cfg_change_t;

typedef struct
{
    int type;
    union
    {
        msg_handshake_t hs;
        msg_handshake_response_t hsr;
        msg_requestvote_t rv;
        msg_requestvote_response_t rvr;
        msg_appendentries_t ae;
        msg_appendentries_response_t aer;
    };
    int padding[100];
} msg_t;

typedef enum
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
} conn_status_e;

struct peer_connection_t
{
    /* peer's address */
    net::ip::address addr;

    uint16_t http_port = 0;
    uint16_t raft_port = 0;

    /* gather TPL message */
    tpl_gather_t *gt = nullptr;

    /* tell if we need to connect or not */
    conn_status_e connection_status = DISCONNECTED;

    /* peer's raft node_idx */
    raft_node_t *node = nullptr;

    /* number of entries currently expected.
     * this counts down as we consume entries */
    int n_expected_entries = 0;

    /* remember most recent append entries msg, we refer to this msg when we
     * finish reading the log entries.
     * used in tandem with n_expected_entries */
    msg_t ae = {0};

    tcp::socket stream;
    std::queue<net::const_buffer> pending;
    std::vector<char> reading;
    decltype(net::dynamic_buffer(reading)) read_buf{reading};
    net::io_context *loop = nullptr;

    peer_connection_t *next = nullptr;

public:
    explicit peer_connection_t(net::io_context &loop_) : stream(loop_), loop(&loop_) {}
};

struct server_t
{
    /* the server's node ID */
    int node_id = 0;

    raft_server_t *raft = nullptr;

    /* Set of tickets that have been issued
     * We store unsigned ints in here */
    MDB_dbi tickets = 0;

    /* Persistent state for voted_for and term
     * We store string keys (eg. "term") with int values */
    MDB_dbi state = 0;

    /* Entries that have been appended to our log
     * For each log entry we store two things next to each other:
     *  - TPL serialized raft_entry_t
     *  - raft_entry_data_t */
    MDB_dbi entries = 0;

    /* LMDB database environment */
    MDB_env *db_env = nullptr;

    /* Raft isn't multi-threaded, therefore we use a global lock */
    std::mutex raft_lock;

    /* When we receive an entry from the client we need to block until the
     * entry has been committed. This condition is used to wake us up. */
    std::condition_variable appendentries_received;

    net::io_context peer_loop{1};
    net::steady_timer periodic_timer{peer_loop};
    tcp::acceptor peer_listen{peer_loop};

    /* Link list of peer connections */
    peer_connection_t *conns = nullptr;

    boost::pool<> pool[64];
    int load_flag = 0; /* 加载标志 */

    int stop_flag = 0;

public:
    server_t() : pool{
                     boost::pool<>(64 * 1),
                     boost::pool<>(64 * 2),
                     boost::pool<>(64 * 3),
                     boost::pool<>(64 * 4),
                     boost::pool<>(64 * 5),
                     boost::pool<>(64 * 6),
                     boost::pool<>(64 * 7),
                     boost::pool<>(64 * 8),
                     boost::pool<>(64 * 9),
                     boost::pool<>(64 * 10),
                     boost::pool<>(64 * 11),
                     boost::pool<>(64 * 12),
                     boost::pool<>(64 * 13),
                     boost::pool<>(64 * 14),
                     boost::pool<>(64 * 15),
                     boost::pool<>(64 * 16),
                     boost::pool<>(64 * 17),
                     boost::pool<>(64 * 18),
                     boost::pool<>(64 * 19),
                     boost::pool<>(64 * 20),
                     boost::pool<>(64 * 21),
                     boost::pool<>(64 * 22),
                     boost::pool<>(64 * 23),
                     boost::pool<>(64 * 24),
                     boost::pool<>(64 * 25),
                     boost::pool<>(64 * 26),
                     boost::pool<>(64 * 27),
                     boost::pool<>(64 * 28),
                     boost::pool<>(64 * 29),
                     boost::pool<>(64 * 30),
                     boost::pool<>(64 * 31),
                     boost::pool<>(64 * 32),
                     boost::pool<>(64 * 33),
                     boost::pool<>(64 * 34),
                     boost::pool<>(64 * 35),
                     boost::pool<>(64 * 36),
                     boost::pool<>(64 * 37),
                     boost::pool<>(64 * 38),
                     boost::pool<>(64 * 39),
                     boost::pool<>(64 * 40),
                     boost::pool<>(64 * 41),
                     boost::pool<>(64 * 42),
                     boost::pool<>(64 * 43),
                     boost::pool<>(64 * 44),
                     boost::pool<>(64 * 45),
                     boost::pool<>(64 * 46),
                     boost::pool<>(64 * 47),
                     boost::pool<>(64 * 48),
                     boost::pool<>(64 * 49),
                     boost::pool<>(64 * 50),
                     boost::pool<>(64 * 51),
                     boost::pool<>(64 * 52),
                     boost::pool<>(64 * 53),
                     boost::pool<>(64 * 54),
                     boost::pool<>(64 * 55),
                     boost::pool<>(64 * 56),
                     boost::pool<>(64 * 57),
                     boost::pool<>(64 * 58),
                     boost::pool<>(64 * 59),
                     boost::pool<>(64 * 60),
                     boost::pool<>(64 * 61),
                     boost::pool<>(64 * 62),
                     boost::pool<>(64 * 63),
                     boost::pool<>(64 * 64),
                 }
    {
    }
};

unsigned int __generate_ticket();


#define UTIL_CAT_I(a, b) a##b
#define UTIL_CAT(a, b) UTIL_CAT_I(a, b)

template <typename F>
class __dummy_defer_t
{
    F f_;

public:
    __dummy_defer_t() = default;
    explicit __dummy_defer_t(F &&f)
        : f_(std::move(f))
    {
    }
    ~__dummy_defer_t()
    {
        f_();
    }

    __dummy_defer_t(const __dummy_defer_t &) = delete;
    __dummy_defer_t(__dummy_defer_t &&) = delete;
    __dummy_defer_t &operator=(const __dummy_defer_t &) = delete;
    __dummy_defer_t &operator=(__dummy_defer_t &&) = delete;
};

#define UTIL_DEFER(...) \
    __dummy_defer_t UTIL_CAT(__dummy, __LINE__)((__VA_ARGS__))

struct __make_defer_t
{
    __make_defer_t() = default;
    __make_defer_t(const __make_defer_t &) = delete;
    __make_defer_t(__make_defer_t &&) = delete;
    __make_defer_t &operator=(const __make_defer_t &) = delete;
    __make_defer_t &operator=(__make_defer_t &&) = delete;

    template <typename F>
    auto operator<<(F &&f) -> __dummy_defer_t<F>
    {
        return __dummy_defer_t(std::forward<F>(f));
    }
};
#define MAKE_DEFER \
    auto UTIL_CAT(__dummy, __LINE__) = __make_defer_t() <<

#endif // SRC_MAIN_H_