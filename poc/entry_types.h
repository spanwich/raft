/* FPT-169: Custom Raft entry types for SemperOS distributed capability system */

#ifndef ENTRY_TYPES_H
#define ENTRY_TYPES_H

#include <stdint.h>
#include <stddef.h>

#include "raft.h"

/* Custom entry types start above RAFT_LOGTYPE_NUM (100).
 * ADD_SERVER and REMOVE_SERVER use the library's built-in
 * RAFT_LOGTYPE_ADD_NONVOTING_NODE / RAFT_LOGTYPE_ADD_NODE /
 * RAFT_LOGTYPE_REMOVE_NODE for Raft membership management.
 * Our custom types carry the application-level state machine data. */
#define ENTRY_MHT_UPDATE       (RAFT_LOGTYPE_NUM + 1)
#define ENTRY_CAP_BLOCK        (RAFT_LOGTYPE_NUM + 2)
#define ENTRY_CAP_UNBLOCK      (RAFT_LOGTYPE_NUM + 3)
#define ENTRY_CAP_EXCHANGE     (RAFT_LOGTYPE_NUM + 4)
#define ENTRY_CAP_GROUP_REVOKE (RAFT_LOGTYPE_NUM + 5)
#define ENTRY_UPDATE_ROOT_CA   (RAFT_LOGTYPE_NUM + 6)

/* --- Payload structs (packed, replicated as-is) --- */

/* MHT_UPDATE: update a Merkle Hash Tree entry for a kernel */
typedef struct {
    uint32_t kernel_id;
    uint32_t cap_count;
    uint8_t  hash[32];
} __attribute__((packed)) mht_update_t;

/* CAP_BLOCK / CAP_UNBLOCK payload */
typedef struct {
    uint64_t cap_id;
    uint32_t pe_id;
    uint8_t  reason;  /* 0=revoked, 1=expired, 2=admin */
} __attribute__((packed)) cap_block_t;

/* CAP_EXCHANGE payload */
typedef struct {
    uint32_t src_kernel;
    uint32_t dst_kernel;
    uint64_t cap_id;
    uint32_t group_id;
} __attribute__((packed)) cap_exchange_t;

/* CAP_GROUP_REVOKE payload */
typedef struct {
    uint32_t group_id;
    uint32_t kernel_id;
} __attribute__((packed)) cap_group_revoke_t;

/* UPDATE_ROOT_CA payload */
typedef struct {
    uint8_t  fingerprint[32];
    uint32_t valid_from;   /* epoch seconds */
    uint32_t valid_until;
} __attribute__((packed)) update_root_ca_t;

/* Payload for built-in membership entries (ADD_NODE / REMOVE_NODE).
 * The library requires get_node_id callback to extract the node id. */
typedef struct {
    int      node_id;
    char     name[32];
    uint8_t  cert_fingerprint[32];
} __attribute__((packed)) membership_data_t;

/* --- Helper functions --- */

/* Create a raft entry with the given type and payload.
 * Returns a new entry with refcount=1. Caller must release. */
raft_entry_t *entry_create(int type, const void *payload, size_t payload_len);

/* Get human-readable name for an entry type */
const char *entry_type_name(int type);

#endif /* ENTRY_TYPES_H */
