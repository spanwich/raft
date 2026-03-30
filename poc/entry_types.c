/* FPT-169: Entry type helpers */

#include "entry_types.h"
#include <string.h>
#include <stdio.h>

raft_entry_t *entry_create(int type, const void *payload, size_t payload_len)
{
    raft_entry_t *ety = raft_entry_new(payload_len);
    if (!ety)
        return NULL;

    ety->type = type;
    if (payload && payload_len > 0)
        memcpy(ety->data, payload, payload_len);

    return ety;
}

const char *entry_type_name(int type)
{
    switch (type) {
    case RAFT_LOGTYPE_NORMAL:             return "NORMAL";
    case RAFT_LOGTYPE_ADD_NONVOTING_NODE: return "ADD_NONVOTING_NODE";
    case RAFT_LOGTYPE_ADD_NODE:           return "ADD_NODE";
    case RAFT_LOGTYPE_REMOVE_NODE:        return "REMOVE_NODE";
    case RAFT_LOGTYPE_NO_OP:              return "NO_OP";
    case ENTRY_MHT_UPDATE:                return "MHT_UPDATE";
    case ENTRY_CAP_BLOCK:                 return "CAP_BLOCK";
    case ENTRY_CAP_UNBLOCK:               return "CAP_UNBLOCK";
    case ENTRY_CAP_EXCHANGE:              return "CAP_EXCHANGE";
    case ENTRY_CAP_GROUP_REVOKE:          return "CAP_GROUP_REVOKE";
    case ENTRY_UPDATE_ROOT_CA:            return "UPDATE_ROOT_CA";
    default:                              return "UNKNOWN";
    }
}
