#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#include <uv.h>
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

typedef struct peer_connection_s peer_connection_t;

struct peer_connection_s
{
    /* peer's address */
    struct sockaddr_in addr;

    int http_port, raft_port;

    /* gather TPL message */
    tpl_gather_t *gt;

    /* tell if we need to connect or not */
    conn_status_e connection_status;

    /* peer's raft node_idx */
    raft_node_t *node;

    /* number of entries currently expected.
     * this counts down as we consume entries */
    int n_expected_entries;

    /* remember most recent append entries msg, we refer to this msg when we
     * finish reading the log entries.
     * used in tandem with n_expected_entries */
    msg_t ae;

    uv_stream_t *stream;

    uv_loop_t *loop;

    peer_connection_t *next;
};

typedef struct
{
    /* the server's node ID */
    int node_id;

    raft_server_t *raft;

    /* Set of tickets that have been issued
     * We store unsigned ints in here */
    MDB_dbi tickets;

    /* Persistent state for voted_for and term
     * We store string keys (eg. "term") with int values */
    MDB_dbi state;

    /* Entries that have been appended to our log
     * For each log entry we store two things next to each other:
     *  - TPL serialized raft_entry_t
     *  - raft_entry_data_t */
    MDB_dbi entries;

    /* LMDB database environment */
    MDB_env *db_env;

    // h2o_globalconf_t cfg;
    // h2o_context_t ctx;
    // h2o_accept_ctx_t accept_ctx;

    /* Raft isn't multi-threaded, therefore we use a global lock */
    uv_mutex_t raft_lock;

    /* When we receive an entry from the client we need to block until the
     * entry has been committed. This condition is used to wake us up. */
    uv_cond_t appendentries_received;

    uv_loop_t peer_loop, http_loop;

    /* Link list of peer connections */
    peer_connection_t *conns;

    int load_flag;  /* 加载标志 */
} server_t;

unsigned int __generate_ticket();

#endif // SRC_MAIN_H_