/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2013.  ALL RIGHTS RESERVED.
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

#include "step.h"

inline void ucp_coll_comp_complete_cb(ucp_coll_step_t *step)
{
    if (step->pending_mask == 0) {
        if (step->del_op) {
            ucp_coll_op_fin(step->del_op);
        } else {
            ucs_status_t error;
            void *next_ptr = step->queue.next;
            ucp_coll_step_t *next_step =
                    ucs_container_of(next_ptr,
                            ucp_coll_step_t, queue);
            if (ucs_unlikely(next_ptr == NULL)) {
                // Amazing! the op completed inside the launch() call!
                step->queue.next = (void*)1; // message for ucp_coll_new_op()...
                return;
            }
            error = ucp_coll_step_launch(next_step);
            ucs_assert(error == UCS_OK);
        }
    }
}

static inline void ucp_coll_comp_complete_reduce_contig(ucp_group_create_params_t *cb_params,
		ucp_coll_reduce_op_h op, void *src_buffer, void *dst_buffer,
        ucp_coll_reduce_datatype_h datatype, unsigned dcount)
{
	cb_params->datatype.reduce(op, src_buffer, dst_buffer, dcount, datatype);
}

static inline size_t ucp_coll_comp_complete_reduce_fragmented(ucp_group_create_params_t *cb_params,
		ucp_coll_reduce_op_h op, void *src_buffer, void *dst_buffer, size_t length, size_t offset,
        ucp_coll_reduce_datatype_h datatype, unsigned dcount, size_t dsize, uint8_t *step_partial_data)
{
    size_t rest = 0;
    ucs_assert(dcount * dsize >= length);

    if (ucs_unlikely(offset % dsize != 0)) {
        // copy rest of datatype and reduce the stored partial data
        size_t existing = offset % dsize;
        rest = dsize - existing;
        memcpy(step_partial_data + existing, src_buffer, rest);
        offset += rest;
        ucp_coll_comp_complete_reduce_contig(cb_params, op,
                step_partial_data, dst_buffer + offset - dsize, datatype, 1);
    }

    dcount = (length - rest) / dsize;
    cb_params->datatype.reduce(op, src_buffer + rest, dst_buffer + offset, dcount, datatype);

    if (ucs_unlikely(dcount * dsize < length)) {
        // copy aside partial datatype if truncated by end of packet
        rest += dcount * dsize;
        memcpy(step_partial_data, src_buffer + rest, length - rest);
    }
    return length;
}

size_t ucp_coll_comp_reduce_single_dst_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;
    ucs_assert(step->num_req == 1);

    ucp_coll_comp_complete_reduce_contig(cb_params, params->reduce.op, buffer,
    		params->rbuf, params->datatype, params->dcount);

    if (ucs_likely(params->dcount * params->dsize == offset + length)) {
        ucp_coll_comp_complete_cb(step);
    }
    return length;
}

size_t ucp_coll_comp_reduce_dst_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
    unsigned incoming = step_cb - step->step_cb_args;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;

    ucp_coll_comp_complete_reduce_contig(cb_params, params->reduce.op, buffer,
    		params->rbuf, params->datatype, params->dcount);

    if (ucs_likely(params->dcount * params->dsize == offset + length)) {
        ucs_assert(UCP_COLL_STEP_IS_PENDING(step, incoming));
        UCP_COLL_STEP_UPDATE_PENDING(step, incoming);
        ucp_coll_comp_complete_cb(step);
    }
    return length;
}

size_t ucp_coll_comp_reduce_single_src_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;
    ucs_assert(step->num_req == 1);

    ucp_coll_comp_complete_reduce_contig(cb_params, params->reduce.op, buffer,
    		params->sbuf, params->datatype, params->dcount);

    if (ucs_likely(params->dcount * params->dsize == offset + length)) {
        ucp_coll_comp_complete_cb(step);
    }
    return length;
}

size_t ucp_coll_comp_reduce_src_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
    unsigned incoming = step_cb - step->step_cb_args;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;

    ucp_coll_comp_complete_reduce_contig(cb_params, params->reduce.op, buffer,
    		params->sbuf, params->datatype, params->dcount);

    if (ucs_likely(params->dcount * params->dsize == offset + length)) {
        ucs_assert(UCP_COLL_STEP_IS_PENDING(step, incoming));
        UCP_COLL_STEP_UPDATE_PENDING(step, incoming);
        ucp_coll_comp_complete_cb(step);
    }
    return length;
}

size_t ucp_coll_comp_reduce_single_dst_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;
    ucs_assert(step->num_req == 1);

    size_t processed = ucp_coll_comp_complete_reduce_fragmented(cb_params,
    		params->reduce.op, buffer, params->rbuf, length, offset,
            params->datatype, params->dcount, params->dsize,
            step->partial_data);

    if (ucs_likely(params->dcount * params->dsize == offset + processed)) {
        ucp_coll_comp_complete_cb(step);
    }
    return processed;
}

size_t ucp_coll_comp_reduce_dst_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
    unsigned incoming = step_cb - step->step_cb_args;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;

    size_t processed = ucp_coll_comp_complete_reduce_fragmented(cb_params,
    		params->reduce.op, buffer, params->rbuf, length, offset,
            params->datatype, params->dcount, params->dsize,
            step->partial_data + incoming * params->dsize);

    if (ucs_likely(params->dcount * params->dsize == offset + processed)) {
        ucs_assert(UCP_COLL_STEP_IS_PENDING(step, incoming));
        UCP_COLL_STEP_UPDATE_PENDING(step, incoming);
        ucp_coll_comp_complete_cb(step);
    }
    return processed;
}

size_t ucp_coll_comp_reduce_single_src_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;
    ucs_assert(step->num_req == 1);

    size_t processed = ucp_coll_comp_complete_reduce_fragmented(cb_params,
    		params->reduce.op, buffer, params->sbuf, length, offset,
            params->datatype, params->dcount, params->dsize,
            step->partial_data);

    if (ucs_likely(params->dcount * params->dsize == offset + processed)) {
        ucp_coll_comp_complete_cb(step);
    }
    return processed;
}

size_t ucp_coll_comp_reduce_src_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
    unsigned incoming = step_cb - step->step_cb_args;
	ucp_group_collective_params_t *params = step->req_params->coll_params;
	ucp_group_create_params_t *cb_params = step->req_params->group_params;

	size_t processed = ucp_coll_comp_complete_reduce_fragmented(cb_params,
    		params->reduce.op, buffer, params->sbuf, length, offset,
            params->datatype, params->dcount, params->dsize,
            step->partial_data + incoming * params->dsize);

    if (ucs_likely(params->dcount * params->dsize == offset + processed)) {
        ucs_assert(UCP_COLL_STEP_IS_PENDING(step, incoming));
        UCP_COLL_STEP_UPDATE_PENDING(step, incoming);
        ucp_coll_comp_complete_cb(step);
    }
    return processed;
}

void ucp_coll_comp_bitfield_complete_cb(ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
    unsigned incoming = step_cb - step->step_cb_args;

    ucs_assert(UCP_COLL_STEP_IS_PENDING(step, incoming));
    UCP_COLL_STEP_UPDATE_PENDING(step, incoming);
    ucp_coll_comp_complete_cb(step);
}

void ucp_coll_comp_single_complete_cb(ucp_coll_step_t **step_cb)
{
    ucp_coll_step_t *step = *step_cb;
    ucs_assert(step->num_req == 1);
    ucp_coll_comp_complete_cb(step);
}
