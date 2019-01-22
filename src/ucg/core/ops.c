/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#include "ops.h"

#include <string.h>

#include <ucs/datastruct/queue.h>
#include <ucs/datastruct/list.h>
#include <ucs/debug/memtrack.h>
#include <ucs/debug/assert.h>
#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_request.inl>
#include <ucp/tag/tag_match.inl>

mpi_reduce_f mpi_reduce;

#define UCG_STEP_IDX_IS_PENDING(step, idx) ((step)->pending & UCS_BIT(idx))
#define UCG_STEP_IDX_UPDATE_PENDING(step, idx) (step)->pending &= ~UCS_BIT(idx)

struct ucg_step_req {
    ucp_request_t    req;
    ucg_step_t *step;
};

static UCS_F_ALWAYS_INLINE void
ucg_comp_request_completed(ucp_request_t *req, ucs_status_t status)
{
    ucs_trace_req("collective returning completed request %p, %s", req,
            ucs_status_string(status));

    req->status = status;
    req->flags |= UCP_REQUEST_FLAG_COMPLETED;
    if (req->flags & UCP_REQUEST_FLAG_RELEASED) {
        ucp_request_put(req);
    }

    UCS_PROFILE_REQUEST_EVENT(req, "complete_coll", 0);
}

static UCS_F_ALWAYS_INLINE void
ucg_comp_common_check(ucg_step_t *step, ucs_status_t status)
{
    /* Check the status, possibly invoking the fault-tolerance callback */
    ucg_op_t *op = step->op;
    ucs_assert(status == UCS_OK); // TODO: remove after debugging is done...
    if (ucs_unlikely(status != UCS_OK)) {
        if (op->params.comp_cb) {
            op->params.comp_cb(op->cb_req, status);
        }
        // TODO: mark op as failed, get this result to the user, or just continue the op?
    }
}

static UCS_F_ALWAYS_INLINE void
ucg_comp_step_cb(ucg_step_t *step, ucs_status_t status)
{
    /* trigger the next step, if it exists */
    ucg_op_t *op = step->op;
    ucs_assert(step == ucs_queue_head_elem_non_empty(&op->step_q, ucg_step_t, queue));
    ucs_queue_pull_non_empty(&op->step_q); /* remove my request */
    if (ucs_queue_is_empty(&op->step_q)) {
        /* Collective operation was completed */
        ucp_request_t *request = (ucp_request_t*)op->cb_req - 1;
        ucg_comp_request_completed(request, status);
        op->cb_req = NULL; /* Mark op as ready again */
    } else {
        /* Start on the next step for this collective operation */
        ucg_step_execute(ucs_queue_head_elem_non_empty(&op->step_q,
                ucg_step_t, queue));
    }
}

static void ucg_comp_send_one_cb(void *cb_data, ucs_status_t status)
{
    ucp_request_t *request = (ucp_request_t*)cb_data - 1;
    ucg_step_req_t *step_comp =
            ucs_container_of(request, ucg_step_req_t, req);
    ucg_step_t *step = step_comp->step;
    ucg_comp_common_check(step, status);
    ucg_comp_step_cb(step, status);
}

static void ucg_comp_recv_one_cb(void *cb_data, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    ucg_comp_send_one_cb(cb_data, status);
}

static void ucg_comp_send_many_cb(void *cb_data, ucs_status_t status)
{
    ucp_request_t *request = (ucp_request_t*)cb_data - 1;
    ucg_step_req_t *step_comp =
            ucs_container_of(request, ucg_step_req_t, req);
    ucg_step_t *step = step_comp->step;
    unsigned cb_idx = step_comp - step->reqs;

    ucg_comp_common_check(step, status);
    ucs_assert(UCG_STEP_IDX_IS_PENDING(step, cb_idx));
    UCG_STEP_IDX_UPDATE_PENDING(step, cb_idx);
    if (step->pending == 0) {
        ucg_comp_step_cb(step, status);
    }
}

static void ucg_comp_recv_many_cb(void *cb_data, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    ucg_comp_send_many_cb(cb_data, status);
}

static void ucg_comp_reduce_one_cb(void *cb_data, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    ucp_request_t *request = (ucp_request_t*)cb_data - 1;
    ucg_step_req_t *step_comp =
            ucs_container_of(request, ucg_step_req_t, req);
    ucg_step_t *step = step_comp->step;

    if (status == UCS_OK) {
        ucg_collective_params_t *params = &step->op->params;
        (void) mpi_reduce(params->cb_r_op,
                ((ucg_step_req_t*)request)->req.recv.buffer,
                step->reduced, params->count, params->cb_r_dtype);
    }

    ucg_comp_common_check(step, status);
    ucg_comp_step_cb(step, status);
}

static void ucg_comp_reduce_many_cb(void *cb_data, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    ucp_request_t *request = (ucp_request_t*)cb_data - 1;
    ucg_step_req_t *step_comp =
            ucs_container_of(request, ucg_step_req_t, req);
    ucg_step_t *step = step_comp->step;
    unsigned cb_idx = step_comp - step->reqs;

    if (status == UCS_OK) {
        ucg_collective_params_t *params = &step->op->params;
        (void) mpi_reduce(params->cb_r_op,
                ((ucg_step_req_t*)request)->req.recv.buffer,
                step->reduced, params->count, params->cb_r_dtype);
    }

    ucg_comp_common_check(step, status);
    ucs_assert(UCG_STEP_IDX_IS_PENDING(step, cb_idx));
    UCG_STEP_IDX_UPDATE_PENDING(step, cb_idx);
    if (step->pending == 0) {
        ucg_comp_step_cb(step, status);
    }
}

static ucs_status_t ucg_step_select_callbacks(ucg_topo_phase_t *phase,
        ucp_send_callback_t *send_cb, ucp_tag_recv_callback_t *recv_cb, int nonzero_length)
{
    switch (phase->method) {
    case UCG_TOPO_METHOD_SEND:
        *send_cb = (phase->ep_cnt > 1) ? ucg_comp_send_many_cb : ucg_comp_send_one_cb;
        break;

    case UCG_TOPO_METHOD_REDUCE:
        *recv_cb = (phase->ep_cnt > 1) ? ucg_comp_reduce_many_cb : ucg_comp_reduce_one_cb;
        if (nonzero_length) break; /* otherwise no reduction needed! */

    case UCG_TOPO_METHOD_REDUCE_RECURSIVE:
        *send_cb = ucg_comp_send_many_cb;
        *recv_cb = ucg_comp_reduce_many_cb;
        if (nonzero_length) break; /* otherwise no reduction needed! */

    case UCG_TOPO_METHOD_RECV:
        *recv_cb = (phase->ep_cnt > 1) ? ucg_comp_recv_many_cb : ucg_comp_recv_one_cb;
        break;

    case UCG_TOPO_METHOD_STABLE_REDUCE:
    case UCG_TOPO_METHOD_STABLE_REDUCE_RECURSIVE:
        ucs_error("Recursive K-ing reduction is not yet supported.");
        return UCS_ERR_NOT_IMPLEMENTED;

    default:
        ucs_error("Invalid method for a collective operation.");
        return UCS_ERR_INVALID_PARAM;
    }

    return UCS_OK;
}


static ucs_status_t ucg_am_short_cb_many_wrapper(uct_pending_req_t *self) {
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucs_status_t status = uct_ep_am_short(ucp_ep_get_am_uct_ep(req->send.ep),
            UCP_AM_ID_EAGER_ONLY, req->send.tag.tag, req->send.buffer, req->send.length);
    ucg_comp_send_many_cb(req + 1, status);
    return status;
}

static ucs_status_t ucg_am_short_cb_one_wrapper(uct_pending_req_t *self) {
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucs_status_t status = uct_ep_am_short(ucp_ep_get_am_uct_ep(req->send.ep),
            UCP_AM_ID_EAGER_ONLY, req->send.tag.tag, req->send.buffer, req->send.length);
    ucg_comp_send_one_cb(req + 1, status);
    return status;
}

static ucs_status_t ucg_tag_eager_short_cb_many_wrapper(uct_pending_req_t *self) {
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucs_status_t status = uct_ep_tag_eager_short(ucp_ep_get_tag_uct_ep(req->send.ep),
            req->send.tag.tag, req->send.buffer, req->send.length);
    ucg_comp_send_many_cb(req + 1, status);
    return status;
}

static ucs_status_t ucg_tag_eager_short_cb_one_wrapper(uct_pending_req_t *self) {
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucs_status_t status = uct_ep_tag_eager_short(ucp_ep_get_tag_uct_ep(req->send.ep),
            req->send.tag.tag, req->send.buffer, req->send.length);
    ucg_comp_send_one_cb(req + 1, status);
    return status;
}

void ucp_tag_send_req_init_wrapper(ucp_request_t* req, ucp_ep_h ep, const void* buffer,
        uintptr_t datatype, size_t count, ucp_tag_t tag, uint16_t flags);

ucs_status_t ucp_tag_send_req_wrapper(ucp_request_t *req, size_t dt_count,
        const ucp_ep_msg_config_t* msg_config,
        size_t rndv_rma_thresh, size_t rndv_am_thresh,
        ucp_send_callback_t cb, const ucp_proto_t *proto,
        int enable_zcopy);

static UCS_F_ALWAYS_INLINE void
ucg_step_create_send_req(ucp_ep_h ep, ucp_request_t *req,
                              ucp_send_callback_t cb,
                              ucg_collective_params_t *params)
{
    /* Initialize the request, which will be triggered when this */
    ucp_tag_send_req_init_wrapper(req, ep, params->sbuf, params->datatype, params->count, 0, 0);
    size_t length = req->send.length; /* set by @ref ucp_tag_send_req_init */

    /* TODO: use...
    int is_data_split = (step->flags & UCG_STEP_FLAG_LENGTH_PER_REQUEST);
    if (is_data_split) {
        params->sbuf += req->send.length;
    }
    */

    /* Check the "inline" send option, like @ref ucp_tag_send_inline */
    if ((ssize_t)length <= ucp_ep_config(ep)->tag.max_eager_short) {
        if (cb == ucg_comp_send_many_cb) {
            req->send.uct.func = ucg_am_short_cb_many_wrapper;
        } else {
            ucs_assert(cb == ucg_comp_send_one_cb);
            req->send.uct.func = ucg_am_short_cb_one_wrapper;
        }
    } else if ((ssize_t)length <= ucp_ep_config(ep)->tag.offload.max_eager_short) {
        if (cb == ucg_comp_send_many_cb) {
            req->send.uct.func = ucg_tag_eager_short_cb_many_wrapper;
        } else {
            ucs_assert(cb == ucg_comp_send_one_cb);
            req->send.uct.func = ucg_tag_eager_short_cb_one_wrapper;
        }
    } else {
        /* Select the best way to send */
        ucs_status_t status = ucp_tag_send_req_wrapper(req,
                params->count, &ucp_ep_config(ep)->tag.eager,
                ucp_ep_config(ep)->tag.rndv_send_nbr.rma_thresh,
                ucp_ep_config(ep)->tag.rndv_send_nbr.am_thresh,
                cb, ucp_ep_config(ep)->tag.proto, 1);
        ucs_assert(status == UCS_OK);
    }
}

void ucp_tag_recv_req_init(ucp_request_t *req, ucp_worker_h worker,
        char *buffer, ucp_datatype_t datatype, unsigned count,
        ucp_tag_t tag, ucp_tag_t tag_mask,
        ucp_tag_recv_callback_t cb, unsigned flags);

static UCS_F_ALWAYS_INLINE void
ucg_step_create_recv_req(ucp_ep_h ep, ucp_request_t *req,
                              ucp_tag_recv_callback_t cb,
                              ucg_collective_params_t *params)
{
    /* These are only temporary holders, for a future ucp_tag_recv_common() */
    req->recv.worker   = ep->worker;
    req->recv.buffer   = params->rbuf;
    req->recv.datatype = params->datatype;
    req->recv.length   = params->count; // Abusing "length" field...
    req->recv.tag.cb   = cb;
}

void ucp_tag_recv_common_wrapper(ucp_worker_h worker, void *buffer, size_t count,
        uintptr_t datatype, ucp_tag_t tag, ucp_tag_t tag_mask,
        ucp_request_t *req, uint16_t req_flags, ucp_tag_recv_callback_t cb,
        ucp_recv_desc_t *rdesc, const char *debug_name);

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_step_execute_recv(ucp_request_t *req)
{
    ucp_recv_desc_t *rdesc = ucp_tag_unexp_search(&req->recv.worker->tm,
            req->recv.tag.tag, req->recv.tag.tag_mask, 1, "coll_recv_nb");
    ucp_tag_recv_common_wrapper(req->recv.worker, req->recv.buffer, req->recv.length,
            req->recv.datatype, req->recv.tag.tag, req->recv.tag.tag_mask,
            req, UCP_REQUEST_FLAG_CALLBACK, req->recv.tag.cb, rdesc, "coll_recv");
    return UCS_OK;
}

ucs_status_t ucg_step_create(ucg_topo_phase_t *phase,
        ucg_collective_params_t *params,
        ucg_group_id_t group_id,
        ucg_step_t *step,
        ucg_step_req_t **cb_ptr)
{
    /* Select the right completion callback */ // TODO: support recv!
    ucp_send_callback_t send_cb;
    ucp_tag_recv_callback_t recv_cb;
    ucs_status_t status = ucg_step_select_callbacks(phase,
            &send_cb, &recv_cb, params->count > 0);
    if (status != UCS_OK) {
        return status;
    }

    step->reduced      = params->rbuf; // TODO: allocate for stable reductions?
    step->flags        = 0; // TODO: set UCG_STEP_FLAG_LENGTH_PER_REQUEST when needed
    step->send_req_cnt = 0;
    step->recv_req_cnt = 0;
    step->reqs         = *cb_ptr;

    /* Create a request for each peer, based on actual buffers/length */
    unsigned ep_idx;
    ucg_step_req_t *req = step->reqs;
    ucp_ep_h ep, *eps = UCG_GET_EPS(phase);
    for (ep_idx = 0, ep = *eps; ep_idx < phase->ep_cnt; req++, ep_idx++, ep = *(eps++)) {
        req->step = step;
        switch (phase->method) {
        case UCG_TOPO_METHOD_SEND:
            ucg_step_create_send_req(ep, &req->req, send_cb, params);
            step->send_req_cnt++;
            break;

        case UCG_TOPO_METHOD_RECV:
        case UCG_TOPO_METHOD_REDUCE:
        case UCG_TOPO_METHOD_STABLE_REDUCE:
            ucg_step_create_recv_req(ep, &req->req, recv_cb, params);
            step->recv_req_cnt++;
            break;

        case UCG_TOPO_METHOD_REDUCE_RECURSIVE:
        case UCG_TOPO_METHOD_STABLE_REDUCE_RECURSIVE:
            ucg_step_create_send_req(ep, &req->req, send_cb, params);
            step->send_req_cnt++;

            req += phase->ep_cnt; /* use the next request as well */
            ucg_step_create_recv_req(ep, &req->req, recv_cb, params);
            step->recv_req_cnt++;

            req->step = step;
            req -= phase->ep_cnt;
            break;

        default:
            ucs_error("Invalid method for a collective operation.");
            return UCS_ERR_INVALID_PARAM;
        }
    }

    *cb_ptr += step->send_req_cnt + step->recv_req_cnt;
    return UCS_OK;
}

void ucg_step_set_tag(ucg_step_t *step, ucp_tag_t tag)
{
    unsigned req_idx;
    ucg_step_req_t *req = step->reqs;
    for (req_idx = 0; req_idx < step->send_req_cnt; req_idx++, req++) {
        req->req.send.tag.tag         = tag;
        req->req.send.state.dt.offset = 0; /* Only matters for bcopy */
    }

    for (req_idx = 0; req_idx < step->recv_req_cnt; req_idx++, req++) {
        req->req.recv.tag.tag         = tag;
        req->req.recv.tag.tag_mask    = (ucp_tag_t)-1; /* full tag match */
    }
}

ucs_status_t ucg_step_execute(ucg_step_t *step)
{
    ucg_step_req_t *req = step->reqs;
    step->pending = UCS_MASK(step->recv_req_cnt + step->send_req_cnt);
    ucs_assert(step == ucs_queue_head_elem_non_empty(&step->op->step_q, ucg_step_t, queue));

    /* Start all the send requests */
    unsigned req_idx;
    for (req_idx = 0; req_idx < step->send_req_cnt; req_idx++, req++) {
        ucs_status_t status = ucp_request_send(&req->req, 0);
        if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {
            return status;
        }
    }

    /* Start all the receive requests */
    for (req_idx = 0; req_idx < step->recv_req_cnt; req_idx++, req++) {
        ucs_status_t status = ucg_step_execute_recv(&req->req);
        if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {
            return status;
        }
    }

    return UCS_OK;
}

ucs_status_t ucg_op_create(ucg_worker_h worker, ucg_topo_t *topo,
        ucg_group_id_t group_id, ucg_collective_params_t *params,
        ucg_op_t **new_op)
{
    /* Allocate the largest possible operation (2 for recursive K-ing) */
    ucg_op_t *op = ucs_malloc(sizeof(ucg_op_t) + topo->phs_cnt *
            (sizeof(ucg_step_t) + 2 * topo->ep_cnt * sizeof(ucg_step_req_t)),
            "ucg_op");
    if (!op) {
        ucs_error("topo->phs_cnt=%u topo->max_ep_cnt=%u", topo->phs_cnt, topo->ep_cnt);
        return UCS_ERR_NO_MEMORY;
    }
    op->worker = worker;
    ucs_queue_head_init(&op->step_q);
    memcpy(&op->params, params, sizeof(*params));

    /* Create a step in the op for each phase in the topology */
    ucg_step_t *next_step = &op->steps[0];
    ucg_topo_phase_t *next_phase = &topo->phss[0];
    ucg_step_req_t *cb_ptr = (ucg_step_req_t*)&op->steps[topo->phs_cnt];
    for (op->step_cnt = 0;
         op->step_cnt < topo->phs_cnt;
         op->step_cnt++, next_step++, next_phase++) {
        ucs_status_t ret = ucg_step_create(next_phase,
                params, group_id, next_step, &cb_ptr);
        if (ret != UCS_OK) {
            ucg_op_destroy(op);
            return ret;
        }
        ucs_queue_push(&op->step_q, &next_step->queue);
        next_step->op = op;
    }

    /* Reduce the resulting op to it's actual size (if unused ep-s remain) */
    *new_op = ucs_realloc(op, (char*)cb_ptr - (char*)op, "ucg_op");
    ucs_assert(*new_op != NULL); /* only reduces size - should never fail */
    return UCS_OK;
}

ucs_status_t ucg_op_recycle(ucg_op_t *op)
{
    int step_idx;
    ucs_queue_head_init(&op->step_q);
    for (step_idx = 0; step_idx < op->step_cnt; step_idx++) {
        ucs_queue_push(&op->step_q, &op->steps[step_idx].queue);
    }

    ucs_list_del(&op->cache_list);
    return UCS_OK;
}

void ucg_op_destroy(ucg_op_t *op)
{
    ucs_free(op);
}
