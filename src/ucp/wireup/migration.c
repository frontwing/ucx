/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "wireup.h"
#include "address.h"
#include "stub_ep.h"
#include "migration.h"

#include <ucp/core/ucp_ep.h>
#include <ucp/core/ucp_request.inl>
#include <ucp/core/ucp_worker.h>
#include <ucs/arch/bitops.h>
#include <ucs/async/async.h>

static ucs_status_t ucp_migration_msg_handler(void *arg, void *data,
                                           size_t length, void *desc)
{
    ucp_worker_h worker   = arg;
    ucp_migration_msg_t *msg = data;
    char peer_name[UCP_WORKER_NAME_MAX];
    ucp_address_entry_t *address_list;
    unsigned address_count;
    ucs_status_t status;
    uint64_t uuid;

    UCS_ASYNC_BLOCK(&worker->async);

    status = ucp_address_unpack(msg + 1, &uuid, peer_name, UCP_WORKER_NAME_MAX,
                                &address_count, &address_list);
    if (status != UCS_OK) {
        ucs_error("failed to unpack address: %s", ucs_status_string(status));
        goto out;
    }

    if (msg->type == UCP_MIGRATION_MSG_STANDBY) {

    } else if (msg->type == UCP_MIGRATION_MSG_STANDBY_ACK) {

    } else if (msg->type == UCP_MIGRATION_MSG_DESTINATION) {

    } else if (msg->type == UCP_MIGRATION_MSG_REDIRECT) {

    } else {
        ucs_bug("invalid migration message");
    }

    ucs_free(address_list);

out:
    UCS_ASYNC_UNBLOCK(&worker->async);
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

//        for (lane = 0; lane < UCP_MAX_LANES; ++lane) {
//            if (msg->tli[lane] == (ae - address_list)) {
//                snprintf(p, end - p, "/lane[%d]", lane);
//                p += strlen(p);
//            }
//        }
    }

    ucs_free(address_list);
}

UCP_DEFINE_AM(-1, UCP_AM_ID_MIGRATION, ucp_migration_msg_handler,
              ucp_migration_msg_dump, UCT_AM_CB_FLAG_ASYNC);

