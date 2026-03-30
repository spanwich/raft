/* FPT-169: Application state machine — mutated by Raft apply callback */

#ifndef APP_STATE_H
#define APP_STATE_H

#include "entry_types.h"

#define MAX_MEMBERS    16
#define MAX_MHT        64
#define MAX_BLOCKLIST  64

typedef struct {
    int      node_id;
    char     name[32];
    uint8_t  cert_fingerprint[32];
    int      active;
} member_t;

typedef struct {
    uint32_t kernel_id;
    uint32_t cap_count;
    uint8_t  hash[32];
} mht_entry_t;

typedef struct {
    /* Membership table */
    int       member_count;
    member_t  members[MAX_MEMBERS];

    /* Stub MHT */
    int         mht_count;
    mht_entry_t mht_entries[MAX_MHT];

    /* Certificate fingerprint blocklist */
    int     blocklist_count;
    uint8_t blocklist[MAX_BLOCKLIST][32];

    /* Counters for stub entry types */
    int cap_block_count;
    int cap_unblock_count;
    int cap_exchange_count;
    int cap_group_revoke_count;
    int root_ca_update_count;

    /* Last applied index (for verification) */
    long last_applied_idx;
} app_state_t;

/* Initialize the application state */
void app_state_init(app_state_t *state);

/* Apply a committed raft entry to the state machine.
 * This is called from the raft applylog callback.
 * Returns 0 on success. */
int app_state_apply(app_state_t *state, raft_entry_t *entry, long idx);

/* Print state summary to stdout */
void app_state_dump(const app_state_t *state);

/* Serialize state to a buffer for comparison across nodes.
 * Returns a malloc'd string; caller frees. */
char *app_state_serialize(const app_state_t *state);

#endif /* APP_STATE_H */
