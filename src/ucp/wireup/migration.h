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


/**
 * Migration message types
 */
enum {
    UCP_MIGRATION_MSG_STANDBY       = 14, /* Peer must pause sends before redirection */
    UCP_MIGRATION_MSG_STANDBY_ACK   = 15, /* Peer acknowledges */
    UCP_MIGRATION_MSG_DESTINATION   = 16, /* New worker sends its address to the original*/
    UCP_MIGRATION_MSG_REDIRECT      = 17, /* Original worker asks peers to redirect */
};

/**
 * Packet structure for wireup requests.
 */
typedef struct ucp_migration_msg {
    uint8_t          type;                /* Message type */



} UCS_S_PACKED ucp_migration_msg_t;

#endif
