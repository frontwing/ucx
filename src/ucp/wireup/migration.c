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

ucs_status_t ucp_proto_progress_migration_msg(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucs_mpool_put(req);
    return UCS_OK;
}

static ucs_status_t ucp_migration_send_complete(ucp_worker_h worker, uint64_t ep_id)
{
    ucp_request_t* req;

    /* Freeze this address */
    ucp_ep_h ep = ucp_worker_ep_find(worker, ep_id);
    // TODO: actually freeze sends

    ucs_trace_req("send_sync_ack sender_uuid %"PRIx64" remote_request 0x%lx",
                      sender_uuid, remote_request);

    /* Send acknowledgement */
    req = ucp_worker_allocate_reply(worker, ep_id);

    req->flags                   = 0;
    req->send.ep                 = ep;
    req->send.proto.am_id        = UCP_AM_ID_MIGRATION;
    req->send.migration.type     = UCP_MIGRATION_MSG_MIGRATE_COMPLETE;
    /* this message has no data */
    req->send.migration.id       = 0;
    req->send.uct.func           = ucp_proto_progress_migration_msg;
    req->send.datatype           = ucp_dt_make_contig(1);

    ep->flags |= UCP_EP_FLAG_DURING_MIGRATION;
    return ucp_request_start_send(req);
}


static ucs_status_t ucp_migration_handle_standby(ucp_worker_h worker, uint64_t ep_id)
{
    ucp_request_t* req;

    ucs_trace_req("send_standby_ack sender_uuid %"PRIx64, ep_id);

    /* Send acknowledgement */
    req = ucp_worker_allocate_reply(worker, ep_id);

    req->flags                   = 0;
    req->send.proto.am_id        = UCP_AM_ID_MIGRATION;
    req->send.migration.type     = UCP_MIGRATION_MSG_STANDBY_ACK;
    req->send.migration.id       = worker->migrations.migration_counter++;
    req->send.uct.func           = ucp_proto_progress_migration_msg;
    req->send.datatype           = ucp_dt_make_contig(1);

    //ep->flags |= UCP_EP_FLAG_DURING_MIGRATION;
    (void) ucp_request_start_send(req);

    return ucp_migration_ep_pause(ucp_worker_ep_find(ep_id));
}

static ucs_status_t ucp_migration_handle_redirect(ucp_worker_h worker,
		uint64_t ep_id, ucp_address_t *address)
{
    ucp_request_t* req;

    /* Send acknowledgement */
    ucs_trace_req("send_redirect_ack sender_uuid %"PRIx64, ep_id);
    req = ucp_worker_allocate_reply(worker, ep_id);

    req->flags                   = 0;
    req->send.proto.am_id        = UCP_AM_ID_MIGRATION;
    req->send.migration.type     = UCP_MIGRATION_MSG_REDIRECT_ACK;
    req->send.uct.func           = ucp_proto_progress_migration_msg;
    req->send.datatype           = ucp_dt_make_contig(1);

    //ep->flags |= UCP_EP_FLAG_DURING_MIGRATION;
    ret_val = ucp_request_start_send(req);

    /* Redirect the connection */
    return ucp_wireup_init_lanes(ucp_worker_ep_find(ep_id),
    		address_count, address_list, addr_indices);
}

static ucs_status_t ucp_migration_send_migrate(ucp_ep_h ep, uint64_t client_id, uint64_t client_uuid, int 

num_clients){
    ucp_request_t* req;

    /* Send migration message */
    req = ucp_worker_allocate_reply(ep->worker, ep->dest_uuid);

    req->flags                   = 0;
    req->send.ep                 = ep;
    req->send.proto.am_id        = UCP_AM_ID_MIGRATION;
    req->send.migration.type     = UCP_MIGRATION_MSG_MIGRATE;
    req->send.migration.id       = client_id;
    req->send.migration.migr_addr.client_uuid = client_uuid;
    req->send.migration.migr_addr.num_clients = num_clients;
    req->send.uct.func           = ucp_proto_progress_migration_msg;
    req->send.datatype           = ucp_dt_make_contig(1);

    ep->flags |= UCP_EP_FLAG_DURING_MIGRATION;
    return ucp_request_start_send(req);
}


static ucs_status_t ucp_migration_send_standby(ucp_ep_h ep){
    ucp_request_t* req;

    /* Send standby message */
    req = ucp_worker_allocate_reply(ep->worker, ep->dest_uuid);

    req->flags                   = 0;
    req->send.ep                 = ep;
    req->send.proto.am_id        = UCP_AM_ID_MIGRATION;
    req->send.migration.type     = UCP_MIGRATION_MSG_STANDBY;
    req->send.uct.func           = ucp_proto_progress_migration_msg;
    req->send.datatype           = ucp_dt_make_contig(1);

    ep->flags |= UCP_EP_FLAG_DURING_MIGRATION;
    return ucp_request_start_send(req);
}


static ucs_status_t ucp_migration_msg_handler(void *arg, void *data,
                                           size_t length, void *desc)
{
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
                // Alex writes this
	        ucp_migration_handle_standby(worker, msg->ep_id);
    } else if (msg->type == UCP_MIGRATION_MSG_STANDBY_ACK) {
	/* This means I am the server and I just got a message from client ACKing my
         * request to migrate client to s2. The ack message has the migration ID created by the client,
         * which needs forwarded to s2, along with client information (address_stuff) */
	// 1. Foreach(client) -> Gather (client address_stuff) plus client ID and pack into single message
	// 2. Foreach(client) -> Send packed message to s2. each message will have the total number of clients
	//      (wasted space,but fine for now)
	// 3. Prepare for the migration_complete message eventaully.
	// Ana writes this

	migration_context->clients_ack++;
	int hash_idx = hashCode(msg->ep_id);  
	migration_context->client_id[hash_idx]->key=msg->id;

    } else if (msg->type == UCP_MIGRATION_MSG_MIGRATE) {
	/* This means I am S2 and I am getting a client ID from s1 (which was generated by the client).
         * s1 should have packed the client information I need to establish a connection with the client
         * in this message. I will now contact each client (using information from S1) */
	/* There is only one client for each single migrate message, but this is what happens: */
	// 1. Foreach(client) -> Unpack (client address_stuff)
	// 2. Foreach(client) -> Establish a connection to the client based on (address_stuff)
	// 3. Foreach(client) -> Create msg_redirect msg with client ID in it and send to each client
	// Brian writes this
	/* address_stuff = unpack message */
	/* get address from endpoint */
	/* create new connection(address_stuff) */
	new_ep = ucp_create_ep(address_stuff)
	/* create msg_redirect, send to client, add client ID in the payload */
	ucp_migration_send_redirect(worker, new_ep, clientID);


    } else if (msg->type == UCP_MIGRATION_MSG_REDIRECT) {
		/* This means I am a client and am getting new "server" information. The new server information is
		 * extractable from the header (doesn't need to be explicit in the payload or anything). I am also sending the
		 * client ID in the payload. I will also prepare the redirect_ack */
    	ucp_migration_handle_redirect(worker, msg->ep_id, (ucp_address_t*)(msg + 1));

	} else if(msg->type == UCP_MIGRATION_MSG_REDIRECT_ACK) {
                /* This means I am s2 and I've had a client acknowledge complete setup of the migration. I need to count
                 * these and ensure I get one per expected client. As soon as I get all of the redirect_acks, I send a
                 * migrate_complete to s1 */
                // 1. Foreach(client) -> mark reception of redirect message
                // 2. When complete, prepare  complete message for S1
                // where do i get s1's data? we have to have it stored somewhere in migration_context maybe?
                // Brian writes this
                migration_context->clients_ack--;
		if(migration_context->clients_ack == 0)
			ucp_migration_send_complete(worker, s1's ep)

    } else if (msg->type == UCP_MIGRATION_MSG_MIGRATE_COMPLETE) {

                /* This means I am s1 and everything has been migrated to s2. I can shut down any client connections I want */
                // 1. Free any resources used for client or S2 communications.
                // Brian writes this
		migration_context->is_complete = 1;
    } else {
        ucs_bug("invalid migration message");
    }

out:
    return UCS_OK;
}

ucs_status_t ucp_migration_send_request(ucp_ep_h ep)
{
//    ucp_worker_h worker = ep->worker;
//    ucp_rsc_index_t rsc_tli[UCP_MAX_LANES];
//    ucp_rsc_index_t rsc_index;
//    uint64_t tl_bitmap = 0;
//    ucp_lane_index_t lane;
    ucs_status_t status;

    if (ep->flags & UCP_EP_FLAG_CONNECT_REQ_SENT) {
        return UCS_OK;
    }

    ucs_assert_always(!ucp_ep_is_stub(ep));

//    for (lane = 0; lane < UCP_MAX_LANES; ++lane) {
//        if (lane < ucp_ep_num_lanes(ep)) {
//            rsc_index = ucp_ep_get_rsc_index(ep, lane);
//            rsc_tli[lane] = ucp_worker_is_tl_p2p(worker, rsc_index) ? rsc_index :
//                                                                      UCP_NULL_RESOURCE;
//            tl_bitmap |= UCS_BIT(rsc_index);
//        } else {
//            rsc_tli[lane] = UCP_NULL_RESOURCE;
//        }
//    }

//    /* TODO make sure such lane would exist */
//    rsc_index = ucp_stub_ep_get_aux_rsc_index(
//                    ep->uct_eps[ucp_ep_get_migration_msg_lane(ep)]);
//    if (rsc_index != UCP_NULL_RESOURCE) {
//        tl_bitmap |= UCS_BIT(rsc_index);
//    }

    ucs_debug("ep %p: send migration request (flags=0x%x)", ep, ep->flags);
//    status = ucp_migration_msg_send(ep, UCP_MIGRATION_MSG_REQUEST, tl_bitmap, rsc_tli);
    ep->flags |= UCP_EP_FLAG_CONNECT_REQ_SENT;
    return status;
}

struct migration_context {
    char clients_acked[MAX_CLIENTS];
};

static int send_standby_to_ep()
{
    status = ucp_migration_msg_send(ep, UCP_MIGRATION_MSG_STANDBY);
}

ucs_status_t ucp_worker_migrate(ucp_worker_h worker, ucp_ep_h target)
{
    /* Send all the clients STANDBY */
    ucp_ep_h ep;
    kh_foreach_value(&worker->ep_hash, ep, ucs_status_t ucp_migration_send_standby(ep));

    /* Waits for client ACKs */
    int num_ep = kh_size(&worker->ep_hash);
    while (migration_context->clients_acked!=num_ep){
	ucp_worker_progress(worker);
    }

    /* Sends <target> the peer addresses */
    int hash_idx = hashCode(ep->dest_uuid);  
    	
    kh_foreach_value(&worker->ep_hash, ep, ucp_migration_send_migrate(target, migration_context->client_id

[hash_idx]->key, ep->dest_uuid, int num_ep));

    /* Wait until the <taget> fnished the migration */
    while (!migration_context->is_complete) {
	ucp_worker_progress(worker);
    }

    return UCS_OK;
}

static void ucp_migration_msg_dump(ucp_worker_h worker, uct_am_trace_type_t type,
                                uint8_t id, const void *data, size_t length,
                                char *buffer, size_t max)
{
    ucp_context_h context       = worker->context;
    const ucp_migration_msg_t *msg = data;
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
             (msg->type == UCP_MIGRATION_MSG_STANDBY     ) ? "STDBY" :
             (msg->type == UCP_MIGRATION_MSG_STANDBY_ACK ) ? "STACK" :
             (msg->type == UCP_MIGRATION_MSG_DESTINATION ) ? "DEST" :
             (msg->type == UCP_MIGRATION_MSG_REDIRECT    ) ? "RDRCT" : "",
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
