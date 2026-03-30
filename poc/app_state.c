/* FPT-169: Application state machine implementation */

#include "app_state.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void app_state_init(app_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

static int apply_add_member(app_state_t *state, raft_entry_t *entry)
{
    if (entry->data_len < sizeof(membership_data_t))
        return 0; /* ignore malformed */

    const membership_data_t *md = (const membership_data_t *)entry->data;

    /* Check if already exists */
    for (int i = 0; i < state->member_count; i++) {
        if (state->members[i].node_id == md->node_id) {
            state->members[i].active = 1;
            return 0;
        }
    }

    if (state->member_count >= MAX_MEMBERS)
        return 0;

    member_t *m = &state->members[state->member_count++];
    m->node_id = md->node_id;
    memcpy(m->name, md->name, sizeof(m->name));
    memcpy(m->cert_fingerprint, md->cert_fingerprint, sizeof(m->cert_fingerprint));
    m->active = 1;

    printf("[STATE] Added member: id=%d name=%s\n", m->node_id, m->name);
    return 0;
}

static int apply_remove_member(app_state_t *state, raft_entry_t *entry)
{
    if (entry->data_len < sizeof(membership_data_t))
        return 0;

    const membership_data_t *md = (const membership_data_t *)entry->data;

    for (int i = 0; i < state->member_count; i++) {
        if (state->members[i].node_id == md->node_id) {
            state->members[i].active = 0;

            /* Add cert fingerprint to blocklist */
            if (state->blocklist_count < MAX_BLOCKLIST) {
                memcpy(state->blocklist[state->blocklist_count],
                       md->cert_fingerprint, 32);
                state->blocklist_count++;
                printf("[STATE] Blocklisted cert for node %d\n", md->node_id);
            }

            printf("[STATE] Removed member: id=%d\n", md->node_id);
            return 0;
        }
    }
    return 0;
}

static int apply_mht_update(app_state_t *state, raft_entry_t *entry)
{
    if (entry->data_len < sizeof(mht_update_t))
        return 0;

    const mht_update_t *upd = (const mht_update_t *)entry->data;

    /* Update existing or insert new */
    for (int i = 0; i < state->mht_count; i++) {
        if (state->mht_entries[i].kernel_id == upd->kernel_id) {
            state->mht_entries[i].cap_count = upd->cap_count;
            memcpy(state->mht_entries[i].hash, upd->hash, 32);
            printf("[STATE] MHT updated: kernel=%u caps=%u\n",
                   upd->kernel_id, upd->cap_count);
            return 0;
        }
    }

    if (state->mht_count >= MAX_MHT)
        return 0;

    mht_entry_t *e = &state->mht_entries[state->mht_count++];
    e->kernel_id = upd->kernel_id;
    e->cap_count = upd->cap_count;
    memcpy(e->hash, upd->hash, 32);
    printf("[STATE] MHT added: kernel=%u caps=%u\n",
           upd->kernel_id, upd->cap_count);
    return 0;
}

int app_state_apply(app_state_t *state, raft_entry_t *entry, long idx)
{
    state->last_applied_idx = idx;

    switch (entry->type) {
    case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
    case RAFT_LOGTYPE_ADD_NODE:
        return apply_add_member(state, entry);

    case RAFT_LOGTYPE_REMOVE_NODE:
        return apply_remove_member(state, entry);

    case RAFT_LOGTYPE_NO_OP:
        printf("[STATE] Applied NO_OP at idx=%ld\n", idx);
        return 0;

    case RAFT_LOGTYPE_NORMAL:
        printf("[STATE] Applied NORMAL at idx=%ld\n", idx);
        return 0;

    case ENTRY_MHT_UPDATE:
        return apply_mht_update(state, entry);

    case ENTRY_CAP_BLOCK:
        state->cap_block_count++;
        printf("[STATE] Applied CAP_BLOCK at idx=%ld (total=%d)\n",
               idx, state->cap_block_count);
        return 0;

    case ENTRY_CAP_UNBLOCK:
        state->cap_unblock_count++;
        printf("[STATE] Applied CAP_UNBLOCK at idx=%ld (total=%d)\n",
               idx, state->cap_unblock_count);
        return 0;

    case ENTRY_CAP_EXCHANGE:
        state->cap_exchange_count++;
        printf("[STATE] Applied CAP_EXCHANGE at idx=%ld (total=%d)\n",
               idx, state->cap_exchange_count);
        return 0;

    case ENTRY_CAP_GROUP_REVOKE:
        state->cap_group_revoke_count++;
        printf("[STATE] Applied CAP_GROUP_REVOKE at idx=%ld (total=%d)\n",
               idx, state->cap_group_revoke_count);
        return 0;

    case ENTRY_UPDATE_ROOT_CA:
        state->root_ca_update_count++;
        printf("[STATE] Applied UPDATE_ROOT_CA at idx=%ld (total=%d)\n",
               idx, state->root_ca_update_count);
        return 0;

    default:
        printf("[STATE] Applied unknown type=%d at idx=%ld\n",
               entry->type, idx);
        return 0;
    }
}

void app_state_dump(const app_state_t *state)
{
    printf("=== Application State ===\n");
    printf("  last_applied_idx: %ld\n", state->last_applied_idx);
    printf("  members (%d):\n", state->member_count);
    for (int i = 0; i < state->member_count; i++) {
        printf("    [%d] id=%d name=%s active=%d\n",
               i, state->members[i].node_id,
               state->members[i].name,
               state->members[i].active);
    }
    printf("  MHT entries (%d):\n", state->mht_count);
    for (int i = 0; i < state->mht_count; i++) {
        printf("    [%d] kernel=%u caps=%u hash=%.8s...\n",
               i, state->mht_entries[i].kernel_id,
               state->mht_entries[i].cap_count,
               "todo");
    }
    printf("  blocklist: %d entries\n", state->blocklist_count);
    printf("  counters: block=%d unblock=%d exchange=%d "
           "group_revoke=%d root_ca=%d\n",
           state->cap_block_count, state->cap_unblock_count,
           state->cap_exchange_count, state->cap_group_revoke_count,
           state->root_ca_update_count);
    printf("=========================\n");
}

char *app_state_serialize(const app_state_t *state)
{
    char *buf = malloc(4096);
    if (!buf) return NULL;

    int off = 0;
    off += snprintf(buf + off, 4096 - off,
                    "applied=%ld members=%d mht=%d blocklist=%d "
                    "block=%d unblock=%d exchange=%d group_revoke=%d root_ca=%d",
                    state->last_applied_idx,
                    state->member_count, state->mht_count,
                    state->blocklist_count,
                    state->cap_block_count, state->cap_unblock_count,
                    state->cap_exchange_count, state->cap_group_revoke_count,
                    state->root_ca_update_count);

    for (int i = 0; i < state->mht_count; i++) {
        off += snprintf(buf + off, 4096 - off, " mht[%u]=%u",
                        state->mht_entries[i].kernel_id,
                        state->mht_entries[i].cap_count);
    }

    return buf;
}
