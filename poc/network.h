/* FPT-169: TCP networking layer for Raft PoC */

#ifndef NETWORK_H
#define NETWORK_H

#include "raft.h"
#include <netinet/in.h>

/* Wire message types */
#define MSG_REQUESTVOTE_REQ   1
#define MSG_REQUESTVOTE_RESP  2
#define MSG_APPENDENTRIES_REQ 3
#define MSG_APPENDENTRIES_RESP 4
#define MSG_SNAPSHOT_REQ      5
#define MSG_SNAPSHOT_RESP     6
#define MSG_TIMEOUTNOW        7

/* Wire header: [msg_type:u8][payload_len:u32] = 5 bytes */
#define WIRE_HEADER_SIZE 5

/* Max message size: 64 KiB should be plenty */
#define MAX_MSG_SIZE (64 * 1024)

/* Per-peer connection state */
typedef struct {
    int              node_id;
    char             host[64];
    int              port;
    int              fd;         /* -1 if not connected */
    /* Receive buffer */
    uint8_t          rxbuf[MAX_MSG_SIZE];
    int              rxlen;
} peer_conn_t;

/* Network context — passed as user_data for raft nodes */
typedef struct {
    int           my_id;
    int           my_port;
    int           listen_fd;
    int           ctrl_fd;       /* control port for test driver */

    int           peer_count;
    peer_conn_t   peers[8];

    /* Back-pointer to raft server (set after init) */
    raft_server_t *raft;
} net_ctx_t;

/* Initialize network: create listen socket, parse peer addresses */
int net_init(net_ctx_t *ctx, int my_id, int my_port, const char *peers_str);

/* Try to connect to all peers that aren't connected */
void net_connect_peers(net_ctx_t *ctx);

/* Poll for incoming messages, dispatch to raft.
 * timeout_ms: poll timeout (-1 for block, 0 for non-blocking) */
int net_poll(net_ctx_t *ctx, int timeout_ms);

/* Accept new peer connections on listen socket */
void net_accept(net_ctx_t *ctx);

/* --- Raft send callbacks --- */

int cb_send_requestvote(raft_server_t *raft, void *user_data,
                        raft_node_t *node, raft_requestvote_req_t *msg);

int cb_send_appendentries(raft_server_t *raft, void *user_data,
                          raft_node_t *node, raft_appendentries_req_t *msg);

int cb_send_snapshot(raft_server_t *raft, void *user_data,
                     raft_node_t *node, raft_snapshot_req_t *msg);

int cb_send_timeoutnow(raft_server_t *raft, void *user_data,
                       raft_node_t *node);

/* Send raw bytes to a peer (by node_id). Returns 0 on success. */
int net_send_to_peer(net_ctx_t *ctx, int node_id,
                     const void *data, size_t len);

/* Find peer_conn_t by node_id */
peer_conn_t *net_find_peer(net_ctx_t *ctx, int node_id);

#endif /* NETWORK_H */
