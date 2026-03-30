/* FPT-169: TCP networking layer implementation */

#include "network.h"
#include "entry_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

/* --- Wire format serialization --- */

static void write_u8(uint8_t *buf, int *off, uint8_t v)
{
    buf[(*off)++] = v;
}

static void write_u32(uint8_t *buf, int *off, uint32_t v)
{
    memcpy(buf + *off, &v, 4);
    *off += 4;
}

static void write_i32(uint8_t *buf, int *off, int32_t v)
{
    memcpy(buf + *off, &v, 4);
    *off += 4;
}

static void write_i64(uint8_t *buf, int *off, int64_t v)
{
    memcpy(buf + *off, &v, 8);
    *off += 8;
}

static void write_u64(uint8_t *buf, int *off, uint64_t v)
{
    memcpy(buf + *off, &v, 8);
    *off += 8;
}

static uint8_t read_u8(const uint8_t *buf, int *off)
{
    return buf[(*off)++];
}

static uint32_t read_u32(const uint8_t *buf, int *off)
{
    uint32_t v;
    memcpy(&v, buf + *off, 4);
    *off += 4;
    return v;
}

static int32_t read_i32(const uint8_t *buf, int *off)
{
    int32_t v;
    memcpy(&v, buf + *off, 4);
    *off += 4;
    return v;
}

static int64_t read_i64(const uint8_t *buf, int *off)
{
    int64_t v;
    memcpy(&v, buf + *off, 8);
    *off += 8;
    return v;
}

static uint64_t read_u64(const uint8_t *buf, int *off)
{
    uint64_t v;
    memcpy(&v, buf + *off, 8);
    *off += 8;
    return v;
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Initialization --- */

int net_init(net_ctx_t *ctx, int my_id, int my_port, const char *peers_str)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->my_id = my_id;
    ctx->my_port = my_port;

    /* Initialize peer fds */
    for (int i = 0; i < 8; i++)
        ctx->peers[i].fd = -1;

    /* Parse peers: "host1:port1,host2:port2,..." */
    if (peers_str && *peers_str) {
        char buf[256];
        strncpy(buf, peers_str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *saveptr;
        char *tok = strtok_r(buf, ",", &saveptr);
        int peer_id = 0;
        while (tok && ctx->peer_count < 8) {
            /* Skip our own ID in the peer numbering */
            if (peer_id == my_id)
                peer_id++;

            char *colon = strrchr(tok, ':');
            if (!colon) {
                fprintf(stderr, "Bad peer format: %s\n", tok);
                tok = strtok_r(NULL, ",", &saveptr);
                continue;
            }

            peer_conn_t *p = &ctx->peers[ctx->peer_count];
            *colon = '\0';
            strncpy(p->host, tok, sizeof(p->host) - 1);
            p->port = atoi(colon + 1);
            p->node_id = peer_id;
            p->fd = -1;
            p->rxlen = 0;

            ctx->peer_count++;
            peer_id++;
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

    /* Create listen socket */
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(ctx->listen_fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(my_port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }

    if (listen(ctx->listen_fd, 8) < 0) {
        perror("listen");
        return -1;
    }

    /* Create control socket (port + 100) */
    ctx->ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(ctx->ctrl_fd);

    addr.sin_port = htons(my_port + 100);
    if (bind(ctx->ctrl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind ctrl");
        return -1;
    }
    listen(ctx->ctrl_fd, 4);

    printf("[NET] Listening on port %d (raft) and %d (ctrl)\n",
           my_port, my_port + 100);
    return 0;
}

peer_conn_t *net_find_peer(net_ctx_t *ctx, int node_id)
{
    for (int i = 0; i < ctx->peer_count; i++) {
        if (ctx->peers[i].node_id == node_id)
            return &ctx->peers[i];
    }
    return NULL;
}

void net_connect_peers(net_ctx_t *ctx)
{
    for (int i = 0; i < ctx->peer_count; i++) {
        peer_conn_t *p = &ctx->peers[i];
        if (p->fd >= 0)
            continue;

        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", p->port);

        if (getaddrinfo(p->host, port_str, &hints, &res) != 0)
            continue;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            freeaddrinfo(res);
            continue;
        }

        set_nonblock(fd);

        int rc = connect(fd, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);

        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }

        /* Send our node_id as a 4-byte handshake */
        int32_t id = ctx->my_id;
        send(fd, &id, 4, MSG_NOSIGNAL);

        p->fd = fd;
        p->rxlen = 0;
        printf("[NET] Connected to peer %d (%s:%d)\n",
               p->node_id, p->host, p->port);
    }
}

void net_accept(net_ctx_t *ctx)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(ctx->listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0)
        return;

    set_nonblock(fd);

    /* Read handshake: 4-byte node_id */
    int32_t remote_id = -1;
    /* Try a blocking-ish read for the handshake */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, &remote_id, 4, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (n != 4 || remote_id < 0) {
        close(fd);
        return;
    }

    peer_conn_t *p = net_find_peer(ctx, remote_id);
    if (!p) {
        printf("[NET] Unknown peer id %d, closing\n", remote_id);
        close(fd);
        return;
    }

    if (p->fd >= 0)
        close(p->fd);

    p->fd = fd;
    p->rxlen = 0;
    printf("[NET] Accepted connection from peer %d\n", remote_id);
}

int net_send_to_peer(net_ctx_t *ctx, int node_id, const void *data, size_t len)
{
    peer_conn_t *p = net_find_peer(ctx, node_id);
    if (!p || p->fd < 0)
        return -1;

    ssize_t sent = send(p->fd, data, len, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            close(p->fd);
            p->fd = -1;
        }
        return -1;
    }
    return 0;
}

/* --- Serialize raft messages to wire format --- */

static int serialize_requestvote(uint8_t *buf, raft_requestvote_req_t *msg)
{
    int off = 0;
    write_u8(buf, &off, MSG_REQUESTVOTE_REQ);
    int len_off = off;
    off += 4; /* placeholder for length */

    write_i32(buf, &off, msg->prevote);
    write_i64(buf, &off, msg->term);
    write_i32(buf, &off, msg->candidate_id);
    write_i64(buf, &off, msg->last_log_idx);
    write_i64(buf, &off, msg->last_log_term);

    uint32_t payload_len = off - WIRE_HEADER_SIZE;
    memcpy(buf + len_off, &payload_len, 4);
    return off;
}

static int serialize_requestvote_resp(uint8_t *buf, raft_requestvote_resp_t *msg)
{
    int off = 0;
    write_u8(buf, &off, MSG_REQUESTVOTE_RESP);
    int len_off = off;
    off += 4;

    write_i32(buf, &off, msg->prevote);
    write_i64(buf, &off, msg->request_term);
    write_i64(buf, &off, msg->term);
    write_i32(buf, &off, msg->vote_granted);

    uint32_t payload_len = off - WIRE_HEADER_SIZE;
    memcpy(buf + len_off, &payload_len, 4);
    return off;
}

static int serialize_appendentries(uint8_t *buf, raft_appendentries_req_t *msg)
{
    int off = 0;
    write_u8(buf, &off, MSG_APPENDENTRIES_REQ);
    int len_off = off;
    off += 4;

    write_i32(buf, &off, msg->leader_id);
    write_u64(buf, &off, msg->msg_id);
    write_i64(buf, &off, msg->term);
    write_i64(buf, &off, msg->prev_log_idx);
    write_i64(buf, &off, msg->prev_log_term);
    write_i64(buf, &off, msg->leader_commit);
    write_i64(buf, &off, msg->n_entries);

    /* Serialize entries */
    for (long i = 0; i < msg->n_entries; i++) {
        raft_entry_t *e = msg->entries[i];
        write_i64(buf, &off, e->term);
        write_i32(buf, &off, e->id);
        write_u64(buf, &off, e->session);
        write_i32(buf, &off, e->type);
        write_u64(buf, &off, e->data_len);
        if (e->data_len > 0) {
            memcpy(buf + off, e->data, e->data_len);
            off += e->data_len;
        }
    }

    uint32_t payload_len = off - WIRE_HEADER_SIZE;
    memcpy(buf + len_off, &payload_len, 4);
    return off;
}

static int serialize_appendentries_resp(uint8_t *buf,
                                        raft_appendentries_resp_t *msg)
{
    int off = 0;
    write_u8(buf, &off, MSG_APPENDENTRIES_RESP);
    int len_off = off;
    off += 4;

    write_u64(buf, &off, msg->msg_id);
    write_i64(buf, &off, msg->term);
    write_i32(buf, &off, msg->success);
    write_i64(buf, &off, msg->current_idx);

    uint32_t payload_len = off - WIRE_HEADER_SIZE;
    memcpy(buf + len_off, &payload_len, 4);
    return off;
}

/* --- Raft send callbacks --- */

int cb_send_requestvote(raft_server_t *raft, void *user_data,
                        raft_node_t *node, raft_requestvote_req_t *msg)
{
    net_ctx_t *ctx = (net_ctx_t *)user_data;
    int node_id = raft_node_get_id(node);

    uint8_t buf[256];
    int len = serialize_requestvote(buf, msg);
    return net_send_to_peer(ctx, node_id, buf, len);
}

int cb_send_appendentries(raft_server_t *raft, void *user_data,
                          raft_node_t *node, raft_appendentries_req_t *msg)
{
    net_ctx_t *ctx = (net_ctx_t *)user_data;
    int node_id = raft_node_get_id(node);

    uint8_t buf[MAX_MSG_SIZE];
    int len = serialize_appendentries(buf, msg);
    if (len > MAX_MSG_SIZE) {
        fprintf(stderr, "[NET] AE too large: %d bytes\n", len);
        return -1;
    }
    return net_send_to_peer(ctx, node_id, buf, len);
}

int cb_send_snapshot(raft_server_t *raft, void *user_data,
                     raft_node_t *node, raft_snapshot_req_t *msg)
{
    /* Snapshots not implemented in PoC */
    (void)raft; (void)user_data; (void)node; (void)msg;
    return 0;
}

int cb_send_timeoutnow(raft_server_t *raft, void *user_data,
                       raft_node_t *node)
{
    net_ctx_t *ctx = (net_ctx_t *)user_data;
    int node_id = raft_node_get_id(node);

    uint8_t buf[WIRE_HEADER_SIZE];
    int off = 0;
    write_u8(buf, &off, MSG_TIMEOUTNOW);
    uint32_t zero = 0;
    write_u32(buf, &off, zero);
    return net_send_to_peer(ctx, node_id, buf, WIRE_HEADER_SIZE);
}

/* --- Deserialize and dispatch incoming messages --- */

static void dispatch_message(net_ctx_t *ctx, peer_conn_t *peer,
                             uint8_t msg_type, const uint8_t *payload,
                             uint32_t payload_len)
{
    int off = 0;
    raft_server_t *raft = ctx->raft;
    raft_node_t *node = raft_get_node(raft, peer->node_id);

    if (!node) {
        /* Might be a node not yet in our config */
        return;
    }

    switch (msg_type) {
    case MSG_REQUESTVOTE_REQ: {
        raft_requestvote_req_t req;
        req.prevote = read_i32(payload, &off);
        req.term = read_i64(payload, &off);
        req.candidate_id = read_i32(payload, &off);
        req.last_log_idx = read_i64(payload, &off);
        req.last_log_term = read_i64(payload, &off);

        raft_requestvote_resp_t resp;
        raft_recv_requestvote(raft, node, &req, &resp);

        /* Send response */
        uint8_t buf[256];
        int len = serialize_requestvote_resp(buf, &resp);
        net_send_to_peer(ctx, peer->node_id, buf, len);
        break;
    }

    case MSG_REQUESTVOTE_RESP: {
        raft_requestvote_resp_t resp;
        resp.prevote = read_i32(payload, &off);
        resp.request_term = read_i64(payload, &off);
        resp.term = read_i64(payload, &off);
        resp.vote_granted = read_i32(payload, &off);

        raft_recv_requestvote_response(raft, node, &resp);
        break;
    }

    case MSG_APPENDENTRIES_REQ: {
        raft_appendentries_req_t req;
        req.leader_id = read_i32(payload, &off);
        req.msg_id = read_u64(payload, &off);
        req.term = read_i64(payload, &off);
        req.prev_log_idx = read_i64(payload, &off);
        req.prev_log_term = read_i64(payload, &off);
        req.leader_commit = read_i64(payload, &off);
        req.n_entries = read_i64(payload, &off);

        /* Deserialize entries */
        raft_entry_t **entries = NULL;
        if (req.n_entries > 0) {
            entries = calloc(req.n_entries, sizeof(raft_entry_t *));
            for (long i = 0; i < req.n_entries; i++) {
                int64_t term = read_i64(payload, &off);
                int32_t id = read_i32(payload, &off);
                uint64_t session = read_u64(payload, &off);
                int32_t type = read_i32(payload, &off);
                uint64_t data_len = read_u64(payload, &off);

                raft_entry_t *e = raft_entry_new(data_len);
                e->term = term;
                e->id = id;
                e->session = session;
                e->type = type;
                if (data_len > 0) {
                    memcpy(e->data, payload + off, data_len);
                    off += data_len;
                }
                entries[i] = e;
            }
            req.entries = entries;
        } else {
            req.entries = NULL;
        }

        raft_appendentries_resp_t resp;
        raft_recv_appendentries(raft, node, &req, &resp);

        /* Release deserialized entries */
        if (entries) {
            for (long i = 0; i < req.n_entries; i++)
                raft_entry_release(entries[i]);
            free(entries);
        }

        /* Send response */
        uint8_t buf[256];
        int len = serialize_appendentries_resp(buf, &resp);
        net_send_to_peer(ctx, peer->node_id, buf, len);
        break;
    }

    case MSG_APPENDENTRIES_RESP: {
        raft_appendentries_resp_t resp;
        resp.msg_id = read_u64(payload, &off);
        resp.term = read_i64(payload, &off);
        resp.success = read_i32(payload, &off);
        resp.current_idx = read_i64(payload, &off);

        raft_recv_appendentries_response(raft, node, &resp);
        break;
    }

    case MSG_TIMEOUTNOW:
        raft_timeout_now(raft);
        break;

    default:
        fprintf(stderr, "[NET] Unknown msg type: %d\n", msg_type);
        break;
    }
}

static void handle_peer_data(net_ctx_t *ctx, peer_conn_t *peer)
{
    /* Read available data into rxbuf */
    int space = MAX_MSG_SIZE - peer->rxlen;
    if (space <= 0)
        return;

    ssize_t n = recv(peer->fd, peer->rxbuf + peer->rxlen, space, 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(peer->fd);
            peer->fd = -1;
            peer->rxlen = 0;
        }
        return;
    }
    peer->rxlen += n;

    /* Process complete messages */
    while (peer->rxlen >= WIRE_HEADER_SIZE) {
        uint8_t msg_type = peer->rxbuf[0];
        uint32_t payload_len;
        memcpy(&payload_len, peer->rxbuf + 1, 4);

        int total = WIRE_HEADER_SIZE + payload_len;
        if (peer->rxlen < total)
            break; /* incomplete message */

        dispatch_message(ctx, peer, msg_type,
                         peer->rxbuf + WIRE_HEADER_SIZE, payload_len);

        /* Shift remaining data */
        int remaining = peer->rxlen - total;
        if (remaining > 0)
            memmove(peer->rxbuf, peer->rxbuf + total, remaining);
        peer->rxlen = remaining;
    }
}

int net_poll(net_ctx_t *ctx, int timeout_ms)
{
    struct pollfd fds[16];
    int nfds = 0;

    /* Listen socket */
    fds[nfds].fd = ctx->listen_fd;
    fds[nfds].events = POLLIN;
    nfds++;

    /* Control socket */
    fds[nfds].fd = ctx->ctrl_fd;
    fds[nfds].events = POLLIN;
    nfds++;

    /* Peer connections */
    for (int i = 0; i < ctx->peer_count; i++) {
        if (ctx->peers[i].fd >= 0) {
            fds[nfds].fd = ctx->peers[i].fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
    }

    int rc = poll(fds, nfds, timeout_ms);
    if (rc <= 0)
        return rc;

    int idx = 0;

    /* Accept on listen socket */
    if (fds[idx].revents & POLLIN)
        net_accept(ctx);
    idx++;

    /* Accept on control socket (handled by caller) */
    idx++;

    /* Read from peers */
    for (int i = 0; i < ctx->peer_count; i++) {
        if (ctx->peers[i].fd >= 0) {
            if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR))
                handle_peer_data(ctx, &ctx->peers[i]);
            idx++;
        }
    }

    return rc;
}
