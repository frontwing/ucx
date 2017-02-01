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

static ucs_status_t ucp_migration_send_msg(ucp_worker_h worker, uint64_t dest_uuid, uint8_t type)
{
    ucp_request_t* req;

    /* Send acknowledgement */
    ucs_trace_req("migration msg type#%i target_uuid %"PRIx64, type, dest_uuid);
    req = ucp_worker_allocate_reply(worker, dest_uuid);

    req->send.proto.am_id           = UCP_AM_ID_MIGRATION;
    req->send.migration.type        = type;
    /* this message has no data */
    req->send.migration.source_uuid = worker->uuid;
    req->send.uct.func              = ucp_migration_progress_msg;
    req->send.datatype              = ucp_dt_make_contig(1);

    return ucp_request_start_send(req);
}

static ucs_status_t ucp_migration_send_msg_with_address(ucp_worker_h worker, uint64_t dest_uuid,
		uint8_t type, migration_id_t id, uint64_t address_uuid) {
	ucp_migration_send_msg();
	ucp_address_pack(); // TODO: add it to the sent message
	return UCS_OK;
}

static ucs_status_t ucp_migration_handle_standby(ucp_worker_h worker, uint64_t ep_id)
{
	ucp_migration_send_msg(worker, ep_id, UCP_MIGRATION_MSG_STANDBY_ACK);

	return ucp_migration_ep_pause(ucp_worker_ep_find(worker, ep_id));
}

static ucs_status_t ucp_migration_send_migrate(ucp_worker_h worker, uint64_t client_id)
{
	return ucp_migration_send_msg_with_address(worker, worker->migration.source.dest_uuid, UCP_MIGRATION_MSG_MIGRATE, client_id, 0);
}

static ucs_status_t ucp_migration_handle_standby_ack(ucp_worker_h worker, uint64_t client_uuid)
{
	(void) ucp_migration_send_msg_with_address(worker, worker->migration.source.dest_uuid, UCP_MIGRATION_MSG_REDIRECT, client_uuid, client_uuid);

	if (++worker->migration.clients_ack == worker->migration.clients_total) {
		ucp_migration_send_migrate(worker, client_uuid);
	}

	return UCS_OK;
}

static ucs_status_t ucp_migration_handle_redirect(ucp_worker_h worker, migration_id_t id,
		uint64_t ep_id, unsigned address_count, const ucp_address_entry_t *address_list)
{
    uint8_t addr_indices[UCP_MAX_LANES];

	(void) ucp_migration_send_msg(worker, ep_id, UCP_MIGRATION_MSG_REDIRECT_ACK);

    /* Redirect the connection */
    return ucp_wireup_init_lanes(ucp_worker_ep_find(worker, id),
    		address_count, address_list, addr_indices);
}

static ucs_status_t ucp_migration_send_redirect(ucp_worker_h worker, ucp_ep_h ep, uint64_t client_id)
{
	return ucp_migration_send_msg_with_address(worker, client_id, UCP_MIGRATION_MSG_REDIRECT, source_id, worker->uuid); // send my address
}

static ucs_status_t ucp_migration_send_standby(ucp_worker_h worker, uint64_t client_id){
	return ucp_migration_send_msg(worker, UCP_MIGRATION_MSG_STANDBY, client_id);
}

static ucs_status_t ucp_migration_send_complete(ucp_worker_h worker){
	return ucp_migration_send_msg(worker, UCP_MIGRATION_MSG_MIGRATE_COMPLETE, worker->migration.destination.source_uuid);
}


static ucs_status_t ucp_migration_msg_handler(void *arg, void *data,
                                           size_t length, void *desc)
{
    ucp_worker_h worker   = arg;
    ucp_migrate_msg_t *msg = data;

    uint8_t addr_indices[UCP_MAX_LANES];
    char peer_name[UCP_WORKER_NAME_MAX];
    ucp_address_entry_t *address_list;
    unsigned address_count;
    ucs_status_t status;
    uint64_t uuid;

    if (msg->type == UCP_MIGRATION_MSG_STANDBY) {
       /* This means I am the client and I just got a message from s1 that s1
		* is going to start migration proceedings. I need to create an ACK message
		* which has a  ID and send it back to s1. s1 sends one of these for each client
		* conneted to it */
		// 0. Stop sending app data to s1. How?
		// 1. Create a migration/client ID. Random uint64_t. Just needs to be locally unique?
		// 2. Send the migration/client ID to s1 via STANDYBY_ACK message
		// 3. Prepare to receive a new message from s2 eventually.
		ucp_migration_handle_standby(worker, msg->ep_id);
    } else if (msg->type == UCP_MIGRATION_MSG_STANDBY_ACK) {
		/* This means I am the server and I just got a message from client ACKing my
		 * request to migrate client to s2. The ack message has the migration ID created by the client,
		 * which needs forwarded to s2, along with client information (address_stuff) */
		// 1. Foreach(client) -> Gather (client address_stuff) plus client ID and pack into single message
		// 2. Foreach(client) -> Send packed message to s2. each message will have the total number of clients
		//      (wasted space,but fine for now)
		// 3. Prepare for the migration_complete message eventaully.
		ucp_migration_handle_standby_ack(worker, msg->ep_id);

    } else if (msg->type == UCP_MIGRATION_MSG_MIGRATE) {
    	/* This means I am S2 and I am getting a client ID from s1 (which was generated by the client).
         * s1 should have packed the client information I need to establish a connection with the client
         * in this message. I will now contact each client (using information from S1) */
		/* There is only one client for each single migrate message, but this is what happens: */
		// 1. Foreach(client) -> Unpack (client address_stuff)
		// 2. Foreach(client) -> Establish a connection to the client based on (address_stuff)
		// 3. Foreach(client) -> Create msg_redirect msg with client ID in it and send to each client
		
		/* I just got info from s1. I need s1's endpoint for later in redirect, so store it now */
    	if (worker->migration.destination.source_uuid == 0) {
    		worker->migration.clients_total = msg->total_clients;
    	}
		worker->migration.destination.source_uuid = msg->ep_id;
		uint64_t client_id = msg->migration_data_t.client_id;

// Alex's code below gets the end point we neet for new connection s2->client.
//		uint64_t dest = data->migration.migr_addr.client_uuid
//		migration_id_t id = data->migration.id;

		/* create new connection here based on the dest */
		ucp_ep_params_t params;
		params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		params.address = msg + 1;
		#warning s2 needs to have new_ep_cnt set to 0 (or memset the whole worker to 0?) Also added & operator since we have an array
		status = ucp_ep_create(worker, &params, &worker->migration.source.new_eps[worker->migration.source.new_ep_cnt++]);
		if (status != UCS_OK) {
			ucs_error("failed to unpack address: %s", ucs_status_string(status));
			goto out;
		}

		/* create msg_redirect, send to client, add client ID in the payload */
		ucp_ep_h new_ep = worker->migration.source.new_eps[worker->migration.source.new_ep_cnt];
		ucp_migration_send_redirect(worker, new_ep, client_id);

    } else if (msg->type == UCP_MIGRATION_MSG_REDIRECT) {
		/* This means I am a client and am getting new "server" information. The new server information is
		 * extractable from the header (doesn't need to be explicit in the payload or anything). I am also sending the
		 * client ID in the payload. I will also prepare the redirect_ack */
        status = ucp_address_unpack(msg + 1, &uuid, peer_name, UCP_WORKER_NAME_MAX,
                                    &address_count, &address_list);
        if (status != UCS_OK) {
            ucs_error("failed to unpack address: %s", ucs_status_string(status));
            goto out;
        }

    	ucp_migration_handle_redirect(worker, msg->ep_id, address_count, address_list);
        ucs_free(address_list);

	} else if(msg->type == UCP_MIGRATION_MSG_REDIRECT_ACK) {
                /* This means I am s2 and I've had a client acknowledge complete setup of the migration. I need to count
                 * these and ensure I get one per expected client. As soon as I get all of the redirect_acks, I send a
                 * migrate_complete to s1 */
                // 1. Foreach(client) -> mark reception of redirect message
                // 2. When complete, prepare  complete message for S1
                // where do i get s1's data? we have to have it stored somewhere in worker->migration maybe?
                // Brian writes this
                #warning Can we decrement this, or should we have a "num_clients" that's set once all clients ack?
                worker->migration.clients_ack--;
		/* Do we have all of the acks? If so, send complete */
		if(worker->migration.clients_ack == 0) {
			worker->migration.destination.source_uuid = 0;
			ucp_migration_send_complete(worker, worker->migration.destination.source_uuid, UCP_MIGRATION_MSG_MIGRATE_COMPLETE); /* cached earlier */
		}

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

#warning this function isn't called
/*
ucs_status_t send_standby_to_ep()
{
    return (ucp_migration_msg_send(ep, UCP_MIGRATION_MSG_STANDBY));
}
*/

ucs_status_t ucp_worker_migrate(ucp_worker_h worker, ucp_ep_h target)
{
    ucp_ep_h ep;

    /* Initialize migration context */
    memset(&worker->migration, 0, sizeof(worker->migration));

    /* Send all the clients STANDBY */
    kh_foreach_value(&worker->ep_hash, ep, ucp_migration_send_standby(worker, ep->dest_uuid));

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
             (msg->type == UCP_MIGRATION_MSG_REDIRECT)         ? "RDRCT" :
             (msg->type == UCP_MIGRATION_MSG_REDIRECT_ACK)     ? "DDACK" :
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
