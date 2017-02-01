/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "migration.h"
#include "address.h"
#include "stub_ep.h"
#include "migration.h"

#include <ucp/core/ucp_ep.h>
#include <ucp/core/ucp_request.inl>
#include <ucp/core/ucp_worker.h>
#include <ucs/arch/bitops.h>
#include <ucs/async/async.h>

static ucs_status_t ucp_migration_ep_pause(ucp_ep_h ep)
{
    ucp_lane_index_t lane;

    UCP_THREAD_CS_ENTER_CONDITIONAL(&ep->worker->mt_lock);

    for (lane = 0; lane < ucp_ep_num_lanes(ep); ++lane) {
        uct_ep_destroy(ep->uct_eps[lane]);
    }

    ep->am_lane = UCP_NULL_LANE;

    UCP_THREAD_CS_EXIT_CONDITIONAL(&ep->worker->mt_lock);
    return UCS_OK;
}

static ucs_status_t ucp_migration_progress_msg(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucs_mpool_put(req);
    return UCS_OK;
}

#define MY_ADDRESS (-1)
static ucs_status_t ucp_migration_send_msg_with_address(ucp_worker_h worker,
                                                        uint64_t dest_uuid,
                                                        uint8_t type,
                                                        migration_id_t id,
                                                        ucp_address_t* append_address,
                                                        size_t append_address_length)
{
    ucp_request_t* req;

    /* Send acknowledgement */
    ucs_trace_req("migration msg type#%i target_uuid %"PRIx64, type, dest_uuid);
    req = ucp_worker_allocate_reply(worker, dest_uuid);

    req->send.proto.am_id           = UCP_AM_ID_MIGRATION;
    req->send.migration.type        = type;
    if (id) {
        req->send.migration.source_uuid = id;
    } else {
        req->send.migration.source_uuid = worker->uuid;
    }
    req->send.uct.func              = ucp_migration_progress_msg;
    req->send.datatype              = ucp_dt_make_contig(1);

    if (append_address) {
        if (append_address_length) {
            req->send.buffer = append_address;
            req->send.length = append_address_length;
        } else {
            /* pack all addresses */
            ucs_status_t status = ucp_worker_get_address(worker,
                    (ucp_address_t**)&req->send.buffer, &req->send.length);
            if (status != UCS_OK) {
                ucs_free(req);
                ucs_error("failed to pack address: %s", ucs_status_string(status));
                return status;
            }
        }
    }

    return ucp_request_start_send(req);
}

static inline ucs_status_t ucp_migration_send_msg(ucp_worker_h worker,
                                           uint64_t dest_uuid,
                                           uint8_t type)
{
    return ucp_migration_send_msg_with_address(worker, dest_uuid, type, 0, 0, 0);
}
ucs_status_t ucp_migration_send_complete(ucp_worker_h worker)
{
    ucp_migration_send_msg(worker, UCP_MIGRATION_MSG_MIGRATE_COMPLETE, worker->migration.destination.source_uuid);
    worker->migration.destination.source_uuid = 0;
    return UCS_OK;
}

static ucs_status_t ucp_migration_msg_handler(void *arg, void *data,
                                           size_t length, void *desc)
{
    ucp_ep_h new_ep;
    ucs_status_t status;

    ucp_worker_h worker   = arg;
    ucp_migrate_msg_t *msg = data;

    if (msg->type == UCP_MIGRATION_MSG_STANDBY) {
       /* This means I am the client and I just got a message from s1 that s1
		* is going to start migration proceedings. I need to create an ACK message
		* which has a  ID and send it back to s1. s1 sends one of these for each client
		* conneted to it */
		// 0. Stop sending app data to s1. How?
		// 1. Create a migration/client ID. Random uint64_t. Just needs to be locally unique?
		// 2. Send the migration/client ID to s1 via STANDYBY_ACK message
		// 3. Prepare to receive a new message from s2 eventually.
	    ucp_migration_send_msg(worker,  msg->ep_id, UCP_MIGRATION_MSG_STANDBY_ACK);
	    return ucp_migration_ep_pause(ucp_worker_ep_find(worker,  msg->ep_id));
    } else if (msg->type == UCP_MIGRATION_MSG_STANDBY_ACK) {
		/* This means I am the server and I just got a message from client ACKing my
		 * request to migrate client to s2. The ack message has the migration ID created by the client,
		 * which needs forwarded to s2, along with client information (address_stuff) */
		// 1. Foreach(client) -> Gather (client address_stuff) plus client ID and pack into single message
		// 2. Foreach(client) -> Send packed message to s2. each message will have the total number of clients
		//      (wasted space,but fine for now)
		// 3. Prepare for the migration_complete message eventaully.
	    ucp_migration_send_msg_with_address(worker,
	            worker->migration.source.dest_uuid, UCP_MIGRATION_MSG_MIGRATE,
	            msg->ep_id, (ucp_address_t*)(msg + 1), length - sizeof(*msg));

    } else if (msg->type == UCP_MIGRATION_MSG_MIGRATE) {
    	/* This means I am S2 and I am getting a client ID from s1 (which was generated by the client).
         * s1 should have packed the client information I need to establish a connection with the client
         * in this message. I will now contact each client (using information from S1) */
		/* There is only one client for each single migrate message, but this is what happens: */
		// 1. Foreach(client) -> Unpack (client address_stuff)
		// 2. Foreach(client) -> Establish a connection to the client based on (address_stuff)
		// 3. Foreach(client) -> Create msg_redirect msg with client ID in it and send to each client
		
        ucp_ep_params_t params;
        params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        params.address = (ucp_address_t*)(msg + 1);

		/* I just got info from s1. I need s1's endpoint for later in redirect, so store it now */
    	if (worker->migration.destination.source_uuid == 0) {
    		worker->migration.clients_total = msg->total_clients;
    	}

		worker->migration.destination.source_uuid = msg->ep_id;
		uint64_t client_id = msg->ep_id;

// Alex's code below gets the end point we neet for new connection s2->client.
//		uint64_t dest = data->migration.migr_addr.client_uuid
//		migration_id_t id = data->migration.id;

		/* create new connection here based on the dest */
		status = ucp_ep_create(worker, &params, &new_ep);
		if (status != UCS_OK) {
			ucs_error("failed to unpack address: %s", ucs_status_string(status));
			goto out;
		}

		/* create msg_redirect, send to client, add client ID in the payload */
		new_ep = worker->migration.source.new_eps[worker->migration.source.new_ep_cnt];
		worker->migration.source.new_eps[worker->migration.source.new_ep_cnt++] = new_ep;
		ucp_wireup_send_request(new_ep, client_id);

    } else if (msg->type == UCP_MIGRATION_MSG_MIGRATE_COMPLETE) {

                /* This means I am s1 and everything has been migrated to s2. I can shut down any client connections I want */
                // 1. Free any resources used for client or S2 communications.
                // Brian writes this
		worker->migration.is_complete = 1;
    } else {
        ucs_bug("invalid migration message");
    }

out:
    return UCS_OK;
}

ucs_status_t ucp_worker_migrate(ucp_worker_h worker, ucp_ep_h target)
{
    ucp_ep_h ep;

    /* Initialize migration context */
    memset(&worker->migration, 0, sizeof(worker->migration));

    /* Send all the clients STANDBY */
    kh_foreach_value(&worker->ep_hash, ep,
                     ucp_migration_send_msg(worker, UCP_MIGRATION_MSG_STANDBY,
                             ep->dest_uuid));

    /* Wait until the <taget> fnished the migration */
    while (!worker->migration.is_complete) {
    	ucp_worker_progress(worker);
    }

    return UCS_OK;
}

static void ucp_migration_msg_dump(ucp_worker_h worker, uct_am_trace_type_t type,
                                uint8_t id, const void *data, size_t length,
                                char *buffer, size_t max)
{
    ucp_context_h context       = worker->context;
    const ucp_migrate_msg_t *msg = data;
    char peer_name[UCP_WORKER_NAME_MAX + 1];
    ucp_address_entry_t *address_list, *ae;
    ucp_tl_resource_desc_t *rsc;
    unsigned address_count;
//    ucp_lane_index_t lane;
    uint64_t uuid;
    char *p, *end;

    ucp_address_unpack(msg + 1, &uuid, peer_name, sizeof(peer_name),
                       &address_count, &address_list);

    p   = buffer;
    end = buffer + max;
    snprintf(p, end - p, "MIGRATION %s [%s uuid 0x%"PRIx64"]",
             (msg->type == UCP_MIGRATION_MSG_STANDBY)          ? "STDBY" :
             (msg->type == UCP_MIGRATION_MSG_STANDBY_ACK)      ? "STACK" :
             (msg->type == UCP_MIGRATION_MSG_MIGRATE)          ? "MIG" :
             (msg->type == UCP_MIGRATION_MSG_MIGRATE_COMPLETE) ? "MIGFIN" : "",
             peer_name, uuid);

    p += strlen(p);
    for (ae = address_list; ae < address_list + address_count; ++ae) {
        for (rsc = context->tl_rscs; rsc < context->tl_rscs + context->num_tls; ++rsc) {
            if (ae->tl_name_csum == rsc->tl_name_csum) {
                snprintf(p, end - p, " "UCT_TL_RESOURCE_DESC_FMT,
                         UCT_TL_RESOURCE_DESC_ARG(&rsc->tl_rsc));
                p += strlen(p);
                break;
            }
        }
        snprintf(p, end - p, "/md[%d]", ae->md_index);
        p += strlen(p);
    }

    ucs_free(address_list);
}

UCP_DEFINE_AM(-1, UCP_AM_ID_MIGRATION, ucp_migration_msg_handler,
              ucp_migration_msg_dump, UCT_AM_CB_FLAG_ASYNC);
