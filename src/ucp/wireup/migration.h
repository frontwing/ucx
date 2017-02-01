/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCP_MIGRATION_H_
#define UCP_MIGRATION_H_

#include <ucp/api/ucp.h>
#include <ucp/core/ucp_context.h>
#include <ucp/core/ucp_ep.h>
#include <ucp/core/ucp_worker.h>
#include <uct/api/uct.h>

typedef uint64_t migration_id_t;

/**
 * Migration message types
 */
enum {
    UCP_MIGRATION_MSG_STANDBY = 0,      /* Peer must pause sends before redirection */
    UCP_MIGRATION_MSG_STANDBY_ACK,      /* Peer acknowledges standby (and pauses) */
    UCP_MIGRATION_MSG_MIGRATE,          /* New worker sends its address to the original*/
    //UCP_MIGRATION_MSG_REDIRECT - OBSOLETE, changed to use WIREUP messages...
    UCP_MIGRATION_MSG_REDIRECT_ACK,     /* Peer acknowledges redirection (and resumes) */
    UCP_MIGRATION_MSG_MIGRATE_COMPLETE, /* Migration is complete! */
};

/**
 * Packet structure for wireup requests.
 */
typedef struct ucp_migrate_msg {
    uint8_t          type;                /* Message type */
    union {
        migration_id_t source_uuid;
        struct {
        	uint64_t ep_id;
        	uint64_t total_clients;
        };
    };
} UCS_S_PACKED ucp_migrate_msg_t;

#endif
