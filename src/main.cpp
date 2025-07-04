/**
 * Copyright (c) 2015, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// #include "h2o.h"
// #include "h2o/http1.h"
// #include "h2o_helpers.h"
// #include "lmdb.h"
#include "lmdb_helpers.h"
// #include "raft.h"
#include "uv_helpers.h"
// #include "uv_multiplex.h"
// #include "tpl.h"
#include "arraytools.h"
#include "main.h"

#include "http_server.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace beast = boost::beast; // from <boost/beast.hpp>
namespace http = beast::http;   // from <boost/beast/http.hpp>
namespace net = boost::asio;    // from <boost/asio.hpp>

#include "usage.c"
#include "parse_addr.c"

namespace
{
    int vasprintf(char **strp, const char *format, va_list ap)
    {
        int len = _vscprintf(format, ap); // 计算所需长度（不包括 '\0'）
        if (len < 0)
        {
            return -1;
        }

        *strp = (char *)malloc(len + 1); // 分配足够空间（含 '\0'）
        if (!*strp)
        {
            return -1;
        }

        int ret = vsprintf_s(*strp, len + 1, format, ap); // 安全格式化
        if (ret < 0)
        {
            free(*strp);
            *strp = NULL;
            return -1;
        }

        return ret;
    }

    int asprintf(char **strp, const char *format, ...)
    {
        va_list ap;
        va_start(ap, format);
        int ret = vasprintf(strp, format, ap);
        va_end(ap);
        return ret;
    }
}

options_t opts;
server_t server;
server_t *sv = &server;

static peer_connection_t *__new_connection(server_t *sv);
static void __connect_to_peer(peer_connection_t *conn);
static void __connection_set_peer(peer_connection_t *conn, char *host, int port);
static void __connect_to_peer_at_host(peer_connection_t *conn, char *host, int port);
static void __start_raft_periodic_timer(server_t *sv);
static int __send_handshake_response(peer_connection_t *conn,
                                     handshake_state_e success,
                                     raft_node_t *leader);
static int __send_leave_response(peer_connection_t *conn);

static void __drop_db(server_t *sv);

/** Serialize a peer message using TPL
 * @param[out] bufs libuv buffer to insert serialized message into
 * @param[out] buf Buffer to write serialized message into */
static size_t __peer_msg_serialize(tpl_node *tn, uv_buf_t *buf, char *data)
{
    size_t sz;
    tpl_pack(tn, 0);
    tpl_dump(tn, TPL_GETSIZE, &sz);
    tpl_dump(tn, TPL_MEM | TPL_PREALLOCD, data, RAFT_BUFLEN);
    tpl_free(tn);
    buf->len = sz;
    buf->base = data;
    return sz;
}

static void __peer_msg_send(uv_stream_t *s, tpl_node *tn, uv_buf_t *buf, char *data)
{
    __peer_msg_serialize(tn, buf, data);
    int e = uv_try_write(s, buf, 1);
    if (e < 0)
        uv_fatal(e);
}

/** Check if the ticket has already been issued
 * @return 0 if not unique; otherwise 1 */
static int __check_if_ticket_exists(const unsigned int ticket)
{
    MDB_txn *txn;

    int e = mdb_txn_begin(sv->db_env, NULL, MDB_RDONLY, &txn);
    if (0 != e)
        mdb_fatal(e);

    MDB_val v, k = {.mv_size = sizeof(ticket), .mv_data = (void *)&ticket};

    e = mdb_get(txn, sv->tickets, &k, &v);
    switch (e)
    {
    case 0:
        break;
    case MDB_NOTFOUND:
        e = mdb_txn_commit(txn);
        if (0 != e)
            mdb_fatal(e);
        return 0;
    default:
        mdb_fatal(e);
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);

    return 1;
}

unsigned int __generate_ticket()
{
    unsigned int ticket;

    do
    {
        // TODO need better random number generator
        ticket = rand();
    } while (__check_if_ticket_exists(ticket));
    return ticket;
}

/** Initiate connection if we are disconnected */
static int __connect_if_needed(peer_connection_t *conn)
{
    if (CONNECTED != conn->connection_status)
    {
        if (DISCONNECTED == conn->connection_status)
            __connect_to_peer(conn);
        return -1;
    }
    return 0;
}

/** Raft callback for sending request vote message */
static int __raft_send_requestvote(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node,
    msg_requestvote_t *m)
{
    peer_connection_t *conn = static_cast<peer_connection_t *>(raft_node_get_udata(node));

    int e = __connect_if_needed(conn);
    if (-1 == e)
        return 0;

    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_REQUESTVOTE,
    msg.rv = *m;
    __peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), bufs, buf);
    return 0;
}

/** Raft callback for sending appendentries message */
static int __raft_send_appendentries(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node,
    msg_appendentries_t *m)
{
    uv_buf_t bufs[3];
    peer_connection_t *conn = static_cast<peer_connection_t *>(raft_node_get_udata(node));

    int e = __connect_if_needed(conn);
    if (-1 == e)
        return 0;

    char buf[RAFT_BUFLEN], *ptr = buf;
    msg_t msg = {};
    msg.type = MSG_APPENDENTRIES;
    msg.ae.term = m->term;
    msg.ae.prev_log_idx = m->prev_log_idx;
    msg.ae.prev_log_term = m->prev_log_term;
    msg.ae.leader_commit = m->leader_commit;
    msg.ae.n_entries = 0;
    if (0 < m->n_entries)
        msg.ae.n_entries = 1;
    ptr += __peer_msg_serialize(tpl_map("S(I$(IIIII))", &msg), bufs, ptr);

    /* appendentries with payload */
    if (0 < m->n_entries)
    {
        tpl_bin tb = {
            .addr = m->entries[0].data.buf,
            .sz = m->entries[0].data.len,
        };

        /* list of entries */
        tpl_node *tn = tpl_map("IIIB",
                               &m->entries[0].id,
                               &m->entries[0].term,
                               &m->entries[0].type,
                               &tb);
        size_t sz;
        tpl_pack(tn, 0);
        tpl_dump(tn, TPL_GETSIZE, &sz);
        e = tpl_dump(tn, TPL_MEM | TPL_PREALLOCD, ptr, RAFT_BUFLEN);
        assert(0 == e);
        bufs[1].len = sz;
        bufs[1].base = ptr;
        e = uv_try_write(conn->stream, bufs, 2);
        if (e < 0)
            uv_fatal(e);

        tpl_free(tn);

        printf("send entry(id: %d, term: %d, type: %d, len: %d)\n",
               m->entries[0].id, m->entries[0].term, m->entries[0].type, m->entries[0].data.len);
    }
    else
    {
        /* keep alive appendentries only */
        e = uv_try_write(conn->stream, bufs, 1);
        if (e < 0)
            uv_fatal(e);
    }

    return 0;
}

static int __raft_send_snapshot(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node)
{
    return 0;
}

static void __delete_connection(server_t *sv, peer_connection_t *conn)
{
    peer_connection_t *prev = NULL;
    if (sv->conns == conn)
        sv->conns = conn->next;
    else if (sv->conns != conn)
    {
        for (prev = sv->conns; prev->next != conn; prev = prev->next)
            ;
        prev->next = conn->next;
    }
    else
        assert(0);

    if (conn->node)
        raft_node_set_udata(conn->node, NULL);

    // TODO: make sure all resources are freed
    free(conn);
}

static peer_connection_t *__find_connection(server_t *sv, const char *host, int raft_port)
{
    peer_connection_t *conn;
    for (conn = sv->conns;
         conn && (0 != strcmp(host, inet_ntoa(conn->addr.sin_addr)) ||
                  conn->raft_port != raft_port);
         conn = conn->next)
        ;
    return conn;
}

static int __offer_cfg_change(server_t *sv,
                              raft_server_t *raft,
                              const unsigned char *data,
                              raft_logtype_e change_type)
{
    entry_cfg_change_t *change = static_cast<entry_cfg_change_t *>((void *)data);
    peer_connection_t *conn = __find_connection(sv, change->host, change->raft_port);

    /* Node is being removed */
    if (RAFT_LOGTYPE_REMOVE_NODE == change_type)
    {
        raft_remove_node(raft, raft_get_node(sv->raft, change->node_id));
        if (conn)
            conn->node = NULL;
        /* __delete_connection(sv, conn); */
        return 0;
    }

    /* Node is being added */
    if (!conn)
    {
        conn = __new_connection(sv);
        __connection_set_peer(conn, change->host, change->raft_port);
    }
    conn->http_port = change->http_port;

    int is_self = change->node_id == sv->node_id;
    // if (is_self)
    // {
    //     conn->node = raft_get_node(raft, sv->node_id);
    //     raft_node_set_udata(conn->node, conn);
    //     return 0;
    // }

    switch (change_type)
    {
    case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
        conn->node = raft_add_non_voting_node(raft, conn, change->node_id, is_self);
        break;
    case RAFT_LOGTYPE_ADD_NODE:
        conn->node = raft_add_node(raft, conn, change->node_id, is_self);
        break;
    default:
        assert(0);
    }

    raft_node_set_udata(conn->node, conn);

    return 0;
}

/** Raft callback for applying an entry to the finite state machine */
static int __raft_applylog(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *ety,
    raft_index_t ety_idx)
{
    MDB_txn *txn;

    MDB_val key = {.mv_size = ety->data.len, .mv_data = ety->data.buf};
    MDB_val val = {.mv_size = 0, .mv_data = (void *)"\0"};

    int e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
        mdb_fatal(e);
#if 0
    /* Check if it's a configuration change */
    if (raft_entry_is_cfg_change(ety))
    {
        entry_cfg_change_t *change = static_cast<entry_cfg_change_t *>(ety->data.buf);
        if (RAFT_LOGTYPE_REMOVE_NODE != ety->type || !raft_is_leader(sv->raft))
            goto commit;

        peer_connection_t *conn = __find_connection(sv, change->host, change->raft_port);
        __send_leave_response(conn);
        goto commit;
    }
#endif
    /* This log affects the ticketd state machine */
    e = mdb_put(txn, sv->tickets, &key, &val, 0);
    switch (e)
    {
    case 0:
        break;
    case MDB_MAP_FULL:
    {
        mdb_txn_abort(txn);
        return -1;
    }
    default:
        mdb_fatal(e);
    }

commit:
    /* We save the commit idx for performance reasons.
     * Note that Raft doesn't require this as it can figure it out itself. */
    e = mdb_puts_int(txn, sv->state, "commit_idx", raft_get_commit_idx(raft));

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);

    return 0;
}

/** Raft callback for saving term field to disk.
 * This only returns when change has been made to disk. */
static int __raft_persist_term(
    raft_server_t *raft,
    void *udata,
    raft_term_t term,
    raft_node_id_t vote)
{
    return mdb_puts_int_commit(sv->db_env, sv->state, "term", term);
}

/** Raft callback for saving voted_for field to disk.
 * This only returns when change has been made to disk. */
static int __raft_persist_vote(
    raft_server_t *raft,
    void *udata,
    const int voted_for)
{
    return mdb_puts_int_commit(sv->db_env, sv->state, "voted_for", voted_for);
}

static void __peer_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf)
{
    buf->len = size;
    buf->base = (char *)malloc(size);
}

static int __append_cfg_change(server_t *sv,
                               raft_logtype_e change_type,
                               char *host,
                               int raft_port, int http_port,
                               int node_id)
{
    entry_cfg_change_t *change = static_cast<entry_cfg_change_t *>(calloc(1, sizeof(*change)));
    change->raft_port = raft_port;
    change->http_port = http_port;
    change->node_id = node_id;
    strcpy(change->host, host);
    change->host[IP_STR_LEN - 1] = 0;

    msg_entry_t entry;
    entry.id = rand();
    entry.data.buf = (void *)change;
    entry.data.len = sizeof(*change);
    entry.type = change_type;
    msg_entry_response_t r;
    int e = raft_recv_entry(sv->raft, &entry, &r);
    if (0 != e)
        return -1;
    return 0;
}

/** Deserialize a single log entry from appendentries message */
static void __deserialize_appendentries_payload(msg_entry_t *out,
                                                peer_connection_t *conn,
                                                void *img,
                                                size_t sz)
{
    tpl_bin tb;
    char *fmt = tpl_peek(TPL_MEM, img, sz);
    tpl_node *tn = tpl_map(fmt,
                           &out->id,
                           &out->term,
                           &out->type,
                           &tb);
    free(fmt);
    tpl_load(tn, TPL_MEM, img, sz);
    tpl_unpack(tn, 0);
    tpl_free(tn);
    out->data.buf = tb.addr;
    out->data.len = tb.sz;
}

/** Parse raft peer traffic using binary protocol, and respond to message */
static int __deserialize_and_handle_msg(void *img, size_t sz, void *data)
{
    peer_connection_t *conn = static_cast<peer_connection_t *>(data);
    msg_t m;
    int e;

    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];

    /* special case: handle appendentries payload */
    if (0 < conn->n_expected_entries)
    {
        msg_entry_t entry;

        __deserialize_appendentries_payload(&entry, conn, img, sz);

        printf("recv entry(id: %d, term: %d, type: %d, len: %d)\n",
               entry.id, entry.term, entry.type, entry.data.len);
        conn->ae.ae.entries = &entry;
        msg_t msg = {.type = MSG_APPENDENTRIES_RESPONSE};
        e = raft_recv_appendentries(sv->raft, conn->node, &conn->ae.ae, &msg.aer);

        /* send response */
        uv_buf_t bufs[1];
        char buf[RAFT_BUFLEN];
        __peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), bufs, buf);

        // free(entry.data.buf);

        conn->n_expected_entries = 0;
        return 0;
    }

    /* deserialize message */
    char *fmt = tpl_peek(TPL_MEM, img, sz);
    tpl_node *tn = tpl_map(fmt, &m);
    tpl_load(tn, TPL_MEM, img, sz);
    tpl_unpack(tn, 0);
    tpl_free(tn);

    switch (m.type)
    {
    case MSG_HANDSHAKE:
    {
        peer_connection_t *nconn = __find_connection(
            sv, inet_ntoa(conn->addr.sin_addr), m.hs.raft_port);
        if (nconn && conn != nconn)
            __delete_connection(sv, nconn);

        conn->connection_status = CONNECTED;
        conn->http_port = m.hs.http_port;
        conn->raft_port = m.hs.raft_port;

        raft_node_t *leader = raft_get_current_leader_node(sv->raft);

        /* Is this peer in our configuration already? */
        raft_node_t *node = raft_get_node(sv->raft, m.hs.node_id);
        if (node)
        {
            raft_node_set_udata(node, conn);
            conn->node = node;
        }

        if (!leader)
        {
            return __send_handshake_response(conn, HANDSHAKE_FAILURE, NULL);
        }
        else if (raft_node_get_id(leader) != sv->node_id)
        {
            return __send_handshake_response(conn, HANDSHAKE_FAILURE, leader);
        }
        else if (node)
        {
            return __send_handshake_response(conn, HANDSHAKE_SUCCESS, NULL);
        }
        else
        {
            int e = __append_cfg_change(sv, RAFT_LOGTYPE_ADD_NONVOTING_NODE,
                                        inet_ntoa(conn->addr.sin_addr),
                                        m.hs.raft_port, m.hs.http_port,
                                        m.hs.node_id);
            if (0 != e)
                return __send_handshake_response(conn, HANDSHAKE_FAILURE, NULL);
            return __send_handshake_response(conn, HANDSHAKE_SUCCESS, NULL);
        }
    }
    break;
    case MSG_HANDSHAKE_RESPONSE:
        if (0 == m.hsr.success)
        {
            conn->http_port = m.hsr.http_port;

            /* We're being redirected to the leader */
            if (m.hsr.leader_port)
            {
                peer_connection_t *nconn =
                    __find_connection(sv, m.hsr.leader_host, m.hsr.leader_port);
                if (!nconn)
                {
                    nconn = __new_connection(sv);
                    printf("Redirecting to %s:%d...\n",
                           m.hsr.leader_host, m.hsr.leader_port);
                    __connect_to_peer_at_host(nconn, m.hsr.leader_host,
                                              m.hsr.leader_port);
                }
            }
        }
        else
        {
            printf("Connected to leader: %s:%d\n",
                   inet_ntoa(conn->addr.sin_addr), conn->raft_port);
            if (!conn->node)
                conn->node = raft_get_node(sv->raft, m.hsr.node_id);
        }
        break;
    case MSG_LEAVE:
    {
        if (!conn->node)
        {
            printf("ERROR: no node\n");
            return 0;
        }
        int e = __append_cfg_change(sv, RAFT_LOGTYPE_REMOVE_NODE,
                                    inet_ntoa(conn->addr.sin_addr),
                                    conn->raft_port,
                                    conn->http_port,
                                    raft_node_get_id(conn->node));
        if (0 != e)
            printf("ERROR: Leave request failed\n");
    }
    break;
    case MSG_LEAVE_RESPONSE:
        __drop_db(sv);
        printf("Shutdown complete. Quitting...\n");
        exit(0);
        break;
    case MSG_REQUESTVOTE:
    {
        msg_t msg = {.type = MSG_REQUESTVOTE_RESPONSE};
        e = raft_recv_requestvote(sv->raft, conn->node, &m.rv, &msg.rvr);
        __peer_msg_send(conn->stream, tpl_map("S(I$(II))", &msg), bufs, buf);
    }
    break;
    case MSG_REQUESTVOTE_RESPONSE:
        e = raft_recv_requestvote_response(sv->raft, conn->node, &m.rvr);
        break;
    case MSG_APPENDENTRIES:
        /* special case: get ready to handle appendentries payload */
        if (0 < m.ae.n_entries)
        {
            conn->n_expected_entries = m.ae.n_entries;
            memcpy(&conn->ae, &m, sizeof(msg_t));
            return 0;
        }
        {
            /* this is a keep alive message */
            msg_t msg = {.type = MSG_APPENDENTRIES_RESPONSE};
            e = raft_recv_appendentries(sv->raft, conn->node, &m.ae, &msg.aer);
            __peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), bufs, buf);
        }
        break;
    case MSG_APPENDENTRIES_RESPONSE:
        e = raft_recv_appendentries_response(sv->raft, conn->node, &m.aer);
        uv_cond_signal(&sv->appendentries_received);
        break;
    default:
        printf("unknown msg\n");
        exit(0);
    }
    return 0;
}

/** Read raft traffic using binary protocol */
static void __peer_read_cb(uv_stream_t *tcp, ssize_t nread, const uv_buf_t *buf)
{
    peer_connection_t *conn = static_cast<peer_connection_t *>(tcp->data);

    if (nread < 0)
        switch (nread)
        {
        case UV__ECONNRESET:
        case UV__EOF:
            conn->connection_status = DISCONNECTED;
            __delete_connection(sv, conn);
            return;
        default:
            uv_fatal(nread);
        }

    if (0 <= nread)
    {
        assert(conn);
        uv_mutex_lock(&sv->raft_lock);
        tpl_gather(TPL_GATHER_MEM, buf->base, nread, &conn->gt,
                   __deserialize_and_handle_msg, conn);
        uv_mutex_unlock(&sv->raft_lock);
    }
}

static void __send_leave(peer_connection_t *conn)
{
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_LEAVE;
    __peer_msg_send(conn->stream, tpl_map("S(I)", &msg), &bufs[0], buf);
}

static void __send_handshake(peer_connection_t *conn)
{
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_HANDSHAKE;
    msg.hs.raft_port = atoi(opts.raft_port);
    msg.hs.http_port = atoi(opts.http_port);
    msg.hs.node_id = sv->node_id;
    __peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), &bufs[0], buf);
}

static int __send_leave_response(peer_connection_t *conn)
{
    if (!conn)
    {
        printf("no connection??\n");
        return -1;
    }
    if (!conn->stream)
        return -1;
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_LEAVE_RESPONSE;
    __peer_msg_send(conn->stream, tpl_map("S(I)", &msg), &bufs[0], buf);
    return 0;
}

static int __send_handshake_response(peer_connection_t *conn,
                                     handshake_state_e success,
                                     raft_node_t *leader)
{
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];

    msg_t msg = {};
    msg.type = MSG_HANDSHAKE_RESPONSE;
    msg.hsr.success = success;
    msg.hsr.leader_port = 0;
    msg.hsr.node_id = sv->node_id;

    /* allow the peer to redirect to the leader */
    if (leader)
    {
        peer_connection_t *leader_conn = static_cast<peer_connection_t *>(raft_node_get_udata(leader));
        if (leader_conn)
        {
            msg.hsr.leader_port = leader_conn->raft_port;
            snprintf(msg.hsr.leader_host, IP_STR_LEN, "%s",
                     inet_ntoa(leader_conn->addr.sin_addr));
        }
    }

    msg.hsr.http_port = atoi(opts.http_port);

    __peer_msg_send(conn->stream, tpl_map("S(I$(IIIIs))", &msg), bufs, buf);

    return 0;
}

/** Raft peer has connected to us.
 * Add them to our list of nodes */
static void __on_peer_connection(uv_stream_t *listener, const int status)
{
    int e;

    if (0 != status)
        uv_fatal(status);

    uv_tcp_t *tcp = static_cast<uv_tcp_t *>(calloc(1, sizeof(uv_tcp_t)));
    e = uv_tcp_init(listener->loop, tcp);
    if (0 != e)
        uv_fatal(e);

    e = uv_accept(listener, (uv_stream_t *)tcp);
    if (0 != e)
        uv_fatal(e);

    peer_connection_t *conn = __new_connection(sv);
    conn->node = NULL;
    conn->loop = listener->loop;
    conn->stream = (uv_stream_t *)tcp;
    tcp->data = conn;

    int namelen = sizeof(conn->addr);
    e = uv_tcp_getpeername(tcp, (struct sockaddr *)&conn->addr, &namelen);
    if (0 != e)
        uv_fatal(e);

    e = uv_read_start((uv_stream_t *)tcp, __peer_alloc_cb, __peer_read_cb);
    if (0 != e)
        uv_fatal(e);
}

/** Our connection attempt to raft peer has succeeded */
static void __on_connection_accepted_by_peer(uv_connect_t *req,
                                             const int status)
{
    peer_connection_t *conn = static_cast<peer_connection_t *>(req->data);
    int e;

    switch (status)
    {
    case 0:
        break;
    case -ECONNREFUSED:
        return;
    default:
        uv_fatal(status);
    }

    __send_handshake(conn);

    int nlen = sizeof(conn->addr);
    e = uv_tcp_getpeername((uv_tcp_t *)req->handle, (struct sockaddr *)&conn->addr, &nlen);
    if (0 != e)
        uv_fatal(e);

    /* start reading from peer */
    conn->connection_status = CONNECTED;
    e = uv_read_start(conn->stream, __peer_alloc_cb, __peer_read_cb);
    if (0 != e)
        uv_fatal(e);
}

static peer_connection_t *__new_connection(server_t *sv)
{
    peer_connection_t *conn = static_cast<peer_connection_t *>(calloc(1, sizeof(peer_connection_t)));
    conn->loop = &sv->peer_loop;
    conn->next = sv->conns;
    sv->conns = conn;
    return conn;
}

/** Connect to raft peer */
static void __connect_to_peer(peer_connection_t *conn)
{
    int e;

    uv_tcp_t *tcp = static_cast<uv_tcp_t *>(calloc(1, sizeof(uv_tcp_t)));
    tcp->data = conn;
    e = uv_tcp_init(conn->loop, tcp);
    if (0 != e)
        uv_fatal(e);

    conn->stream = (uv_stream_t *)tcp;
    conn->connection_status = CONNECTING;

    uv_connect_t *c = static_cast<uv_connect_t *>(calloc(1, sizeof(uv_connect_t)));
    c->data = conn;

    e = uv_tcp_connect(c, (uv_tcp_t *)conn->stream,
                       (struct sockaddr *)&conn->addr,
                       __on_connection_accepted_by_peer);
    if (0 != e)
        uv_fatal(e);
}

static void __connection_set_peer(peer_connection_t *conn, char *host, int port)
{
    conn->raft_port = port;
    printf("Connecting to %s:%d\n", host, port);
    int e = uv_ip4_addr(host, port, &conn->addr);
    if (0 != e)
        uv_fatal(e);
}

static void __connect_to_peer_at_host(peer_connection_t *conn, char *host,
                                      int port)
{
    __connection_set_peer(conn, host, port);
    __connect_to_peer(conn);
}

/** Raft callback for displaying debugging information */
void __raft_log(raft_server_t *raft, raft_node_t *node, void *udata,
                const char *buf)
{
    if (opts.debug)
        printf("raft: %s\n", buf);
}

/** Raft callback for appending an item to the log */
static int __raft_logentry_offer(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *ety,
    raft_index_t ety_idx)
{
    MDB_txn *txn;

    // if (raft_entry_is_cfg_change(ety))
    //     __offer_cfg_change(sv, raft, static_cast<const unsigned char *>(ety->data.buf), static_cast<raft_logtype_e>(ety->type));

    int e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
        mdb_fatal(e);

    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    __peer_msg_serialize(tpl_map("S(III)", ety), bufs, buf);

    /* 1. put metadata */
    ety_idx <<= 1;
    MDB_val key = {.mv_size = sizeof(ety_idx), .mv_data = (void *)&ety_idx};
    MDB_val val = {.mv_size = bufs->len, .mv_data = bufs->base};

    e = mdb_put(txn, sv->entries, &key, &val, 0);
    switch (e)
    {
    case 0:
        break;
    case MDB_MAP_FULL:
    {
        mdb_txn_abort(txn);
        return -1;
    }
    default:
        mdb_fatal(e);
    }

    /* 2. put entry */
    ety_idx |= 1;
    key.mv_size = sizeof(ety_idx);
    key.mv_data = (void *)&ety_idx;
    val.mv_size = ety->data.len;
    val.mv_data = ety->data.buf;

    e = mdb_put(txn, sv->entries, &key, &val, 0);
    switch (e)
    {
    case 0:
        break;
    case MDB_MAP_FULL:
    {
        mdb_txn_abort(txn);
        return -1;
    }
    default:
        mdb_fatal(e);
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);
#if 0
    /* So that our entry points to a valid buffer, get the mmap'd buffer.
     * This is because the currently pointed to buffer is temporary. */
    e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
        mdb_fatal(e);

    e = mdb_get(txn, sv->entries, &key, &val);
    switch (e)
    {
    case 0:
        break;
    default:
        mdb_fatal(e);
    }
    ety->data.buf = val.mv_data;
    ety->data.len = val.mv_size;

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);
#endif
    return 0;
}

/** Raft callback for removing the first entry from the log
 * @note this is provided to support log compaction in the future */
static int __raft_logentry_poll(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *entry,
    raft_index_t ety_idx)
{
    MDB_val k, v;

    mdb_poll(sv->db_env, sv->entries, &k, &v);

    return 0;
}

/** Raft callback for deleting the most recent entry from the log.
 * This happens when an invalid leader finds a valid leader and has to delete
 * superseded log entries. */
static int __raft_logentry_pop(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *entry,
    raft_index_t ety_idx)
{
    MDB_val k, v;

    mdb_pop(sv->db_env, sv->entries, &k, &v);

    return 0;
}

static int __raft_log_clear(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *entry,
    raft_index_t ety_idx)
{
    if (entry && entry->data.buf)
    {
        free(entry->data.buf);
        entry->data.buf = nullptr;
        entry->data.len = 0;
    }
    return 0;
}

/** Callback for determining which node this configuration log entry
 * affects. This call only applies to configuration change log entries.
 * @return the node ID of the node */
static int __raft_log_get_node_id(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *entry,
    raft_index_t ety_idx)
{
    // return sv->node_id;
    entry_cfg_change_t *change = static_cast<entry_cfg_change_t *>((void *)entry->data.buf);
    return change->node_id;
}

/** Non-voting node now has enough logs to be able to vote.
 * Append a finalization cfg log entry. */
static int __raft_node_has_sufficient_logs(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node)
{
    peer_connection_t *conn = static_cast<peer_connection_t *>(raft_node_get_udata(node));
    return __append_cfg_change(sv, RAFT_LOGTYPE_ADD_NODE,
                               inet_ntoa(conn->addr.sin_addr),
                               conn->raft_port,
                               conn->http_port,
                               raft_node_get_id(conn->node));
}

static void
__raft_notify_membership_event(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node,
    raft_entry_t *entry,
    raft_membership_e type)
{
    entry_cfg_change_t *change = nullptr;
    peer_connection_t *conn = nullptr;
    switch (type)
    {
    case RAFT_MEMBERSHIP_ADD:
        if (!entry)
        {
            break;
        }
        change = static_cast<entry_cfg_change_t *>(entry->data.buf);
        conn = __find_connection(sv, change->host, change->raft_port);
        if (!conn)
        {
            conn = __new_connection(sv);
            __connection_set_peer(conn, change->host, change->raft_port);
        }
        conn->node = node;
        conn->http_port = change->http_port;
        raft_node_set_udata(node, conn);
        break;
    case RAFT_MEMBERSHIP_REMOVE:
        conn = (peer_connection_t *)raft_node_get_udata(node);
        __send_leave_response(conn);
        raft_node_set_udata(node, NULL);
        break;
    }
    return;
}

raft_cbs_t raft_funcs = {
    .send_requestvote = __raft_send_requestvote,
    .send_appendentries = __raft_send_appendentries,
    .send_snapshot = __raft_send_snapshot,
    .applylog = __raft_applylog,
    .persist_vote = __raft_persist_vote,
    .persist_term = __raft_persist_term,
    .log_offer = __raft_logentry_offer,
    .log_poll = __raft_logentry_poll,
    .log_pop = __raft_logentry_pop,
    .log_clear = __raft_log_clear,
    .log_get_node_id = __raft_log_get_node_id,
    .node_has_sufficient_logs = __raft_node_has_sufficient_logs,
    .notify_membership_event = __raft_notify_membership_event,
    .log = __raft_log,
};

/** Raft callback for handling periodic logic */
static void __periodic(uv_timer_t *handle)
{
    uv_mutex_lock(&sv->raft_lock);

    printf("raft_get_num_voting_nodes: %d/%d\n", raft_get_num_voting_nodes(sv->raft), raft_get_num_nodes(sv->raft));

    raft_periodic(sv->raft, PERIOD_MSEC);

    if (opts.leave)
    {
        raft_node_t *leader = raft_get_current_leader_node(sv->raft);
        if (leader)
        {
            peer_connection_t *leader_conn = static_cast<peer_connection_t *>(raft_node_get_udata(leader));
            assert(raft_node_get_id(leader) != sv->node_id);
            __send_leave(leader_conn);
        }
    }

    // raft_apply_all(sv->raft);

    uv_mutex_unlock(&sv->raft_lock);
}

/** Load all log entries we have persisted to disk */
static void __load_commit_log(server_t *sv)
{
    MDB_cursor *curs;
    MDB_txn *txn;
    MDB_val k, v;
    int e;

    e = mdb_txn_begin(sv->db_env, NULL, MDB_RDONLY, &txn);
    if (0 != e)
        mdb_fatal(e);

    e = mdb_cursor_open(txn, sv->entries, &curs);
    if (0 != e)
        mdb_fatal(e);

    e = mdb_cursor_get(curs, &k, &v, MDB_FIRST);
    switch (e)
    {
    case 0:
        break;
    case MDB_NOTFOUND:
        return;
    default:
        mdb_fatal(e);
    }

    raft_entry_t ety;
    ety.id = 0;

    int n_entries = 0;

    do
    {
        if (!(*(int *)k.mv_data & 1))
        {
            /* entry metadata */
            tpl_node *tn =
                tpl_map(tpl_peek(TPL_MEM, v.mv_data, v.mv_size), &ety);
            tpl_load(tn, TPL_MEM, v.mv_data, v.mv_size);
            tpl_unpack(tn, 0);
        }
        else
        {
            /* entry data for FSM */
            ety.data.buf = v.mv_data;
            ety.data.len = v.mv_size;
            raft_append_entry(sv->raft, &ety);
            n_entries++;
        }

        e = mdb_cursor_get(curs, &k, &v, MDB_NEXT);
    } while (0 == e);

    mdb_cursor_close(curs);

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);

    MDB_val val;
    mdb_gets(sv->db_env, sv->state, "commit_idx", &val);
    if (val.mv_data)
        raft_set_commit_idx(sv->raft, *(int *)val.mv_data);

    raft_apply_all(sv->raft);
}

/** Load voted_for and term raft fields */
static void __load_persistent_state(server_t *sv)
{
    int val = -1;

    mdb_gets_int(sv->db_env, sv->state, "voted_for", &val);
    raft_vote_for_nodeid(sv->raft, val);
    mdb_gets_int(sv->db_env, sv->state, "term", &val);
    raft_set_current_term(sv->raft, val);
}

static int __load_opts(server_t *sv, options_t *opts)
{
    int http_port, raft_port;

    int e = mdb_gets_int(sv->db_env, sv->state, "id", &sv->node_id);
    if (-1 == e)
        return e;

    e = mdb_gets_int(sv->db_env, sv->state, "http_port", &http_port);
    if (-1 == e)
        abort();

    e = mdb_gets_int(sv->db_env, sv->state, "raft_port", &raft_port);
    if (-1 == e)
        abort();

    free(opts->http_port);
    free(opts->raft_port);
    asprintf(&opts->http_port, "%d", http_port);
    asprintf(&opts->raft_port, "%d", raft_port);
    return 0;
}

static void __drop_db(server_t *sv)
{
    MDB_dbi dbs[] = {sv->entries, sv->tickets, sv->state};
    mdb_drop_dbs(sv->db_env, dbs, len(dbs));
}

static void __start_raft_periodic_timer(server_t *sv)
{
    uv_timer_t *periodic_req = static_cast<uv_timer_t *>(calloc(1, sizeof(uv_timer_t)));
    periodic_req->data = sv;
    uv_timer_init(&sv->peer_loop, periodic_req);
    uv_timer_start(periodic_req, __periodic, 0, PERIOD_MSEC);
    raft_set_election_timeout(sv->raft, 2000);
}

static void __int_handler(int dummy)
{
    uv_mutex_lock(&sv->raft_lock);
    raft_node_t *leader = raft_get_current_leader_node(sv->raft);
    if (leader)
    {
        if (raft_node_get_id(leader) == sv->node_id)
        {
            printf("I'm the leader, I can't leave the cluster...\n");
            goto done;
        }

        peer_connection_t *leader_conn = static_cast<peer_connection_t *>(raft_node_get_udata(leader));
        if (leader_conn)
        {
            printf("Leaving cluster...\n");
            __send_leave(leader_conn);
            goto done;
        }
    }
    printf("Try again no leader at the moment...\n");
done:
    uv_mutex_unlock(&sv->raft_lock);
}

static void __new_db(server_t *sv)
{
    mdb_db_env_create(&sv->db_env, 0, opts.path, atoi(opts.db_size));
    mdb_db_create(&sv->entries, sv->db_env, "entries");
    mdb_db_create(&sv->tickets, sv->db_env, "docs");
    mdb_db_create(&sv->state, sv->db_env, "state");
}

static void __start_peer_socket(server_t *sv, const char *host, int port, uv_tcp_t *listen)
{
    memset(&sv->peer_loop, 0, sizeof(uv_loop_t));
    int e = uv_loop_init(&sv->peer_loop);
    if (0 != e)
        uv_fatal(e);

    uv_bind_listen_socket(listen, host, port, &sv->peer_loop);
    e = uv_listen((uv_stream_t *)listen, MAX_PEER_CONNECTIONS,
                  __on_peer_connection);
    if (0 != e)
        uv_fatal(e);
}

static void __save_opts(server_t *sv, options_t *opts)
{
    mdb_puts_int_commit(sv->db_env, sv->state, "id", sv->node_id);
    mdb_puts_int_commit(sv->db_env, sv->state, "raft_port", atoi(opts->raft_port));
    mdb_puts_int_commit(sv->db_env, sv->state, "http_port", atoi(opts->http_port));
}

int main(int argc, char **argv)
{
    memset(sv, 0, sizeof(server_t));

    int e = parse_options(argc, argv, &opts);
    if (-1 == e)
        exit(-1);
    else if (opts.help)
    {
        show_usage();
        exit(0);
    }
    else if (opts.version)
    {
        fprintf(stdout, "%s\n", VERSION);
        exit(0);
    }

    // signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, __int_handler);

    sv->raft = raft_new();
    raft_set_callbacks(sv->raft, &raft_funcs, sv);

    srand(time(NULL));

    __new_db(sv);

    if (opts.drop)
    {
        __drop_db(sv);
        exit(0);
    }

    http_server http_srv;

    /* lock and condition to support HTTP client blocking */
    uv_mutex_init(&sv->raft_lock);
    uv_cond_init(&sv->appendentries_received);

    uv_tcp_t http_listen, peer_listen;
    // uv_multiplex_t m;

    /* get ID */
    if (opts.start || opts.join)
    {
        if (opts.id)
            sv->node_id = atoi(opts.id);
    }
    else
    {
        e = __load_opts(sv, &opts);
        if (0 != e)
        {
            printf("ERROR: No database available.\n"
                   "Please start or join a cluster.\n");
            abort();
        }
    }

    /* add self */
    // raft_add_node(sv->raft, NULL, sv->node_id, 1);
    // raft_node_t* node = raft_get_node(sv->raft, sv->node_id);
    // raft_add_non_voting_node(sv->raft, NULL, sv->node_id, 1);

    if (opts.start || opts.join)
    {
        __drop_db(sv);
        __new_db(sv);
        __save_opts(sv, &opts);

        // __start_http_socket(sv, opts.host, atoi(opts.http_port), &http_listen, &m);
        http_srv.start(sv, opts.host, opts.http_port, 4, false);
        __start_peer_socket(sv, opts.host, atoi(opts.raft_port), &peer_listen);

        if (opts.start)
        {
            // peer_connection_t *conn = __new_connection(sv);
            // __connection_set_peer(conn, opts.host, atoi(opts.raft_port));
            // conn->http_port = atoi(opts.http_port);
            // conn->node = node;
            // raft_add_node(sv->raft, NULL, sv->node_id, 1);
            raft_add_non_voting_node(sv->raft, NULL, sv->node_id, 1);
            // raft_node_set_udata(node, conn);
            // raft_node_set_voting(node, 0);
            raft_become_leader(sv->raft);

            /* We store membership configuration inside the Raft log.
             * This configuration change is going to be the initial membership
             * configuration (ie. original node) inside the Raft log. The
             * first configuration is for a cluster of 1 node. */
            __append_cfg_change(sv, RAFT_LOGTYPE_ADD_NODE,
                                opts.host,
                                atoi(opts.raft_port),
                                atoi(opts.http_port),
                                sv->node_id);

            // raft_node_set_voting(node, 1);
            // raft_node_set_voting(conn->node, 1);
            // raft_node_set_active(sv->raft, 1);

            // raft_set_commit_idx(sv->raft, raft_get_current_idx(sv->raft));
        }
        else
        {
            // raft_add_node(sv->raft, NULL, sv->node_id, 1);
            raft_add_non_voting_node(sv->raft, NULL, sv->node_id, 1);
            // raft_become_follower(sv->raft);

            addr_parse_result_t res;
            parse_addr(opts.PEER, strlen(opts.PEER), &res);
            res.host[res.host_len] = '\0';

            peer_connection_t *conn = __new_connection(sv);
            __connect_to_peer_at_host(conn, res.host, atoi(res.port));
        }
    }
    /* Reload cluster information and rejoin cluster */
    else
    {
        // __start_http_socket(sv, opts.host, atoi(opts.http_port), &http_listen, &m);
        http_srv.start(sv, opts.host, opts.http_port, 4, false);
        __start_peer_socket(sv, opts.host, atoi(opts.raft_port), &peer_listen);
        __load_commit_log(sv);
        __load_persistent_state(sv);

        if (1 == raft_get_num_nodes(sv->raft))
        {
            raft_become_leader(sv->raft);
        }
        else
        {
            for (int i = 0; i < raft_get_num_nodes(sv->raft); i++)
            {
                raft_node_t *node = raft_get_node_from_idx(sv->raft, i);
                if (raft_node_get_id(node) == sv->node_id)
                    continue;
                __connect_to_peer(static_cast<peer_connection_t *>(raft_node_get_udata(node)));
            }
        }
    }

    __start_raft_periodic_timer(sv);

    uv_run(&sv->peer_loop, UV_RUN_DEFAULT);
}
