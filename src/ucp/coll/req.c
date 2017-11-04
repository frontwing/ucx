/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2012.  ALL RIGHTS RESERVED.
* This software product is a proprietary product of Mellanox Technologies Ltd.
* (the "Company") and all right, title, and interest and to the software product,
* including all associated intellectual property rights, are and shall
* remain exclusively with the Company.
*
* This software product is governed by the End User License Agreement
* provided with the software product.
* $COPYRIGHT$
* $HEADER$
*/

#include "req.h"
#include <uct/api/uct.h>

ucs_status_t ucp_coll_req_fill(ucp_coll_req_params_t *params, ucp_coll_req_t *reqs)
{
	// TODO: put UCT-selection logic (e.g. bcopy vs. zcopy) here
	/*
    unsigned i, req_cnt = params->map->ep_cnt;
    for (i = 0; i < req_cnt; i++) {
        ucp_coll_req_t *new = &reqs[i];
        new->ep = params->group->conns[i].ep;
        new->data = params->coll_params.data;
        new->length = params->coll_params.length;
        if (IS_LENGTH_PER_REQ(params->flags)) {
            params->data += params->length;
        }
    }*/ // TODO: Fill in!
    return UCS_OK;
}

/* Triggers the start of a given request */
ucs_status_t ucp_coll_req_trigger(ucp_coll_req_t *req)
{
    switch (req->method) {
    case UCP_COLL_METHOD_SEND_TM_EAGER_SHORT:
        return req->func.ep_tag_eager_short(req->ep, req->tag, req->data, req->length);

    case UCP_COLL_METHOD_SEND_TM_EAGER_BCOPY:
        return req->func.ep_tag_eager_bcopy(req->ep, req->tag, req->imm, req->pack_cb, req->arg);

    case UCP_COLL_METHOD_SEND_TM_EAGER_ZCOPY:
        return req->func.ep_tag_eager_zcopy(req->ep, req->tag, req->imm, req->iov, req->iovcnt, req->comp);

    case UCP_COLL_METHOD_SEND_TM_RNDV_ZCOPY:
        return (ucs_status_t)req->func.ep_tag_rndv_zcopy(req->ep, req->tag, NULL, 0, req->iov, req->iovcnt, req->comp);

    case UCP_COLL_METHOD_RECV_TM_ZCOPY:
        return req->func.iface_tag_recv_zcopy(req->iface, req->tag, req->tag, req->iov, req->iovcnt, req->ctx);

    default:
        return UCS_ERR_INVALID_PARAM;
    }
}
