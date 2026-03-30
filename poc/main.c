/* FPT-169: Raft PoC — 3-node cluster with custom entry types
 *
 * Usage: ./raft_poc --id N --port PORT --peers host1:port1,host2:port2
 *
 * Control port: PORT+100 — accepts simple text commands:
 *   status               -> "id=N leader=L term=T commit=C state=S"
 *   submit MHT_UPDATE K C -> submit MHT_UPDATE for kernel K with cap_count C
 *   submit CAP_BLOCK      -> submit a stub CAP_BLOCK entry
 *   submit CAP_EXCHANGE   -> submit a stub CAP_EXCHANGE entry
 *   dump                 -> dump full app_state
 *   state_hash           -> serialized state for comparison
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#include "raft.h"
#include "entry_types.h"
#include "app_state.h"
#include "network.h"

static volatile int running = 1;
static app_state_t app_state;
static net_ctx_t net_ctx;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

/* --- Raft callbacks --- */

static raft_time_t cb_timestamp(raft_server_t *raft, void *user_data)
{
    (void)raft; (void)user_data;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (raft_time_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static int cb_applylog(raft_server_t *raft, void *user_data,
                       raft_entry_t *entry, raft_index_t entry_idx)
{
    (void)raft; (void)user_data;
    printf("[RAFT] Apply: idx=%ld type=%s(%d) data_len=%llu\n",
           entry_idx, entry_type_name(entry->type), entry->type,
           (unsigned long long)entry->data_len);
    return app_state_apply(&app_state, entry, entry_idx);
}

static int cb_persist_metadata(raft_server_t *raft, void *user_data,
                               raft_term_t term, raft_node_id_t vote)
{
    (void)raft; (void)user_data;
    /* RAM-only — just log it */
    printf("[RAFT] Persist metadata: term=%ld vote=%d\n", term, vote);
    return 0;
}

static raft_node_id_t cb_get_node_id(raft_server_t *raft, void *user_data,
                                     raft_entry_t *entry, raft_index_t idx)
{
    (void)raft; (void)user_data; (void)idx;
    if (entry->data_len >= sizeof(membership_data_t)) {
        const membership_data_t *md = (const membership_data_t *)entry->data;
        return md->node_id;
    }
    return -1;
}

static int cb_node_has_sufficient_logs(raft_server_t *raft, void *user_data,
                                       raft_node_t *node)
{
    (void)raft; (void)user_data;
    printf("[RAFT] Node %d has sufficient logs\n", raft_node_get_id(node));
    return 0;
}

static void cb_log(raft_server_t *raft, void *user_data, const char *buf)
{
    (void)raft; (void)user_data;
    printf("[RAFT-LIB] %s\n", buf);
}

static void cb_notify_state(raft_server_t *raft, void *user_data,
                            raft_state_e state)
{
    (void)user_data;
    const char *names[] = { "?", "FOLLOWER", "PRECANDIDATE",
                            "CANDIDATE", "LEADER" };
    int s = (state >= 1 && state <= 4) ? state : 0;
    printf("[RAFT] State changed to %s (term=%ld)\n",
           names[s], raft_get_current_term(raft));
}

static void cb_notify_membership(raft_server_t *raft, void *user_data,
                                 raft_node_t *node, raft_entry_t *entry,
                                 raft_membership_e type)
{
    (void)raft; (void)user_data; (void)entry;
    int nid = node ? raft_node_get_id(node) : -1;
    printf("[RAFT] Membership event: node=%d type=%s\n",
           nid, type == RAFT_MEMBERSHIP_ADD ? "ADD" : "REMOVE");
}

/* --- Control port handler --- */

static void handle_ctrl_command(net_ctx_t *ctx, int client_fd, char *cmd)
{
    raft_server_t *raft = ctx->raft;
    char resp[4096];

    /* Strip newline */
    char *nl = strchr(cmd, '\n');
    if (nl) *nl = '\0';
    nl = strchr(cmd, '\r');
    if (nl) *nl = '\0';

    if (strncmp(cmd, "status", 6) == 0) {
        snprintf(resp, sizeof(resp),
                 "id=%d leader=%d term=%ld commit=%ld applied=%ld state=%s\n",
                 ctx->my_id,
                 raft_get_leader_id(raft),
                 raft_get_current_term(raft),
                 raft_get_commit_idx(raft),
                 raft_get_last_applied_idx(raft),
                 raft_get_state_str(raft));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "submit MHT_UPDATE", 17) == 0) {
        uint32_t kernel = 0, caps = 0;
        sscanf(cmd + 17, " %u %u", &kernel, &caps);

        mht_update_t payload = { .kernel_id = kernel, .cap_count = caps };
        memset(payload.hash, (uint8_t)(kernel & 0xFF), 32);

        raft_entry_t *ety = entry_create(ENTRY_MHT_UPDATE,
                                         &payload, sizeof(payload));
        raft_entry_resp_t entry_resp;
        int rc = raft_recv_entry(raft, ety, &entry_resp);
        raft_entry_release(ety);

        snprintf(resp, sizeof(resp), "rc=%d idx=%ld err=%s\n",
                 rc, entry_resp.idx, raft_get_error_str(rc));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "submit CAP_BLOCK", 16) == 0) {
        cap_block_t payload = { .cap_id = 42, .pe_id = 1, .reason = 0 };
        raft_entry_t *ety = entry_create(ENTRY_CAP_BLOCK,
                                         &payload, sizeof(payload));
        raft_entry_resp_t entry_resp;
        int rc = raft_recv_entry(raft, ety, &entry_resp);
        raft_entry_release(ety);

        snprintf(resp, sizeof(resp), "rc=%d idx=%ld err=%s\n",
                 rc, entry_resp.idx, raft_get_error_str(rc));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "submit CAP_UNBLOCK", 18) == 0) {
        cap_block_t payload = { .cap_id = 42, .pe_id = 1, .reason = 0 };
        raft_entry_t *ety = entry_create(ENTRY_CAP_UNBLOCK,
                                         &payload, sizeof(payload));
        raft_entry_resp_t entry_resp;
        int rc = raft_recv_entry(raft, ety, &entry_resp);
        raft_entry_release(ety);

        snprintf(resp, sizeof(resp), "rc=%d idx=%ld err=%s\n",
                 rc, entry_resp.idx, raft_get_error_str(rc));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "submit CAP_EXCHANGE", 19) == 0) {
        cap_exchange_t payload = { .src_kernel = 0, .dst_kernel = 1,
                                   .cap_id = 99, .group_id = 1 };
        raft_entry_t *ety = entry_create(ENTRY_CAP_EXCHANGE,
                                         &payload, sizeof(payload));
        raft_entry_resp_t entry_resp;
        int rc = raft_recv_entry(raft, ety, &entry_resp);
        raft_entry_release(ety);

        snprintf(resp, sizeof(resp), "rc=%d idx=%ld err=%s\n",
                 rc, entry_resp.idx, raft_get_error_str(rc));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "submit CAP_GROUP_REVOKE", 23) == 0) {
        cap_group_revoke_t payload = { .group_id = 1, .kernel_id = 0 };
        raft_entry_t *ety = entry_create(ENTRY_CAP_GROUP_REVOKE,
                                         &payload, sizeof(payload));
        raft_entry_resp_t entry_resp;
        int rc = raft_recv_entry(raft, ety, &entry_resp);
        raft_entry_release(ety);

        snprintf(resp, sizeof(resp), "rc=%d idx=%ld err=%s\n",
                 rc, entry_resp.idx, raft_get_error_str(rc));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "submit UPDATE_ROOT_CA", 21) == 0) {
        update_root_ca_t payload;
        memset(payload.fingerprint, 0xCA, 32);
        payload.valid_from = 1000000;
        payload.valid_until = 2000000;
        raft_entry_t *ety = entry_create(ENTRY_UPDATE_ROOT_CA,
                                         &payload, sizeof(payload));
        raft_entry_resp_t entry_resp;
        int rc = raft_recv_entry(raft, ety, &entry_resp);
        raft_entry_release(ety);

        snprintf(resp, sizeof(resp), "rc=%d idx=%ld err=%s\n",
                 rc, entry_resp.idx, raft_get_error_str(rc));
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "dump", 4) == 0) {
        app_state_dump(&app_state);
        snprintf(resp, sizeof(resp), "ok\n");
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else if (strncmp(cmd, "state_hash", 10) == 0) {
        char *s = app_state_serialize(&app_state);
        snprintf(resp, sizeof(resp), "%s\n", s ? s : "error");
        free(s);
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    else {
        snprintf(resp, sizeof(resp), "unknown command: %s\n", cmd);
        send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
}

/* --- Main --- */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s --id N --port PORT --peers host:port,host:port\n",
            prog);
    exit(1);
}

int main(int argc, char **argv)
{
    int my_id = -1;
    int my_port = 9000;
    const char *peers_str = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--id") == 0 && i + 1 < argc)
            my_id = atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            my_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peers") == 0 && i + 1 < argc)
            peers_str = argv[++i];
        else
            usage(argv[0]);
    }

    if (my_id < 0 || !peers_str)
        usage(argv[0]);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    /* Seed random for raft entry IDs */
    srand(time(NULL) ^ (my_id * 1000));

    printf("[MAIN] Starting node %d on port %d\n", my_id, my_port);

    /* Initialize app state */
    app_state_init(&app_state);

    /* Initialize networking */
    if (net_init(&net_ctx, my_id, my_port, peers_str) < 0) {
        fprintf(stderr, "Failed to init network\n");
        return 1;
    }

    /* Create raft server */
    raft_server_t *raft = raft_new();
    if (!raft) {
        fprintf(stderr, "Failed to create raft server\n");
        return 1;
    }

    net_ctx.raft = raft;

    /* Configure raft */
    raft_config(raft, 1, RAFT_CONFIG_AUTO_FLUSH, 1);
    raft_config(raft, 1, RAFT_CONFIG_LOG_ENABLED, 1);
    raft_config(raft, 1, RAFT_CONFIG_ELECTION_TIMEOUT, 1000);
    raft_config(raft, 1, RAFT_CONFIG_REQUEST_TIMEOUT, 200);

    /* Set callbacks */
    raft_cbs_t cbs = {
        .send_requestvote     = cb_send_requestvote,
        .send_appendentries   = cb_send_appendentries,
        .send_snapshot        = cb_send_snapshot,
        .send_timeoutnow      = cb_send_timeoutnow,
        .applylog             = cb_applylog,
        .persist_metadata     = cb_persist_metadata,
        .get_node_id          = cb_get_node_id,
        .node_has_sufficient_logs = cb_node_has_sufficient_logs,
        .timestamp            = cb_timestamp,
        .notify_state_event   = cb_notify_state,
        .notify_membership_event = cb_notify_membership,
        .log                  = cb_log,
    };
    raft_set_callbacks(raft, &cbs, &net_ctx);

    /* Add nodes (ourselves + peers) */
    /* We assume a 3-node cluster with IDs 0, 1, 2 */
    int total_nodes = net_ctx.peer_count + 1;
    for (int id = 0; id < total_nodes; id++) {
        int is_self = (id == my_id);
        raft_node_t *node = raft_add_node(raft, NULL, id, is_self);
        if (!node) {
            fprintf(stderr, "Failed to add node %d\n", id);
            return 1;
        }

        /* Attach peer connection as node udata */
        if (!is_self) {
            peer_conn_t *p = net_find_peer(&net_ctx, id);
            if (p)
                raft_node_set_udata(node, p);
        }
    }

    printf("[MAIN] Cluster configured: %d nodes, my_id=%d\n",
           total_nodes, my_id);

    /* Main event loop */
    int ctrl_client_fd = -1;
    struct timespec last_periodic;
    clock_gettime(CLOCK_MONOTONIC, &last_periodic);

    while (running) {
        /* Try connecting to peers */
        net_connect_peers(&net_ctx);

        /* Poll for network events (50ms timeout) */
        net_poll(&net_ctx, 50);

        /* Accept control connections */
        {
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);
            int fd = accept(net_ctx.ctrl_fd, (struct sockaddr *)&addr, &len);
            if (fd >= 0) {
                if (ctrl_client_fd >= 0)
                    close(ctrl_client_fd);
                ctrl_client_fd = fd;
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }

        /* Read control commands */
        if (ctrl_client_fd >= 0) {
            char cmd[256];
            ssize_t n = recv(ctrl_client_fd, cmd, sizeof(cmd) - 1, 0);
            if (n > 0) {
                cmd[n] = '\0';
                handle_ctrl_command(&net_ctx, ctrl_client_fd, cmd);
            } else if (n == 0) {
                close(ctrl_client_fd);
                ctrl_client_fd = -1;
            }
        }

        /* Call raft_periodic */
        int rc = raft_periodic(raft);
        if (rc == RAFT_ERR_SHUTDOWN) {
            printf("[MAIN] Raft requested shutdown\n");
            break;
        }
    }

    printf("[MAIN] Shutting down\n");
    app_state_dump(&app_state);

    if (ctrl_client_fd >= 0)
        close(ctrl_client_fd);
    raft_destroy(raft);
    close(net_ctx.listen_fd);
    close(net_ctx.ctrl_fd);

    return 0;
}
