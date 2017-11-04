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

//extern memcpy_f ucp_coll_memcpy_f;

ucs_status_t ucp_coll_step_launch(ucp_coll_step_t *step)
{
    unsigned req_idx;
    ucs_status_t error;

    ucp_coll_req_flags_t flags = step->req_params->flags;
    if ((step->num_req == 1) && (!UCP_COLL_REQ_IS_MULTIROOT(flags))) {
        return ucp_coll_req_trigger(&step->reqs[0]);
    }

    step->pending_mask = UCS_BIT(step->num_req) - 1;
    for (req_idx = 0; req_idx < step->num_req; req_idx++) {
    	error = ucp_coll_req_trigger(&step->reqs[req_idx]);
    	if (ucs_unlikely((error != UCS_OK) &&
    			(error != UCS_INPROGRESS))) {
    		return error;
    	}
    }
    return UCS_OK;
}

void ucp_coll_step_destroy(ucp_coll_step_t *step)
{
    if (step->step_cb_args) {
        ucs_free(step->step_cb_args);
    }
    ucs_free(step);
}

ucs_status_t ucp_coll_direct_one(ucp_coll_req_params_t *params,
        ucp_coll_step_t **step)
{
    ucp_coll_step_t *new_step =
            UCS_ALLOC_CHECK(sizeof(*step) + sizeof(ucp_coll_req_t), "step");
    *step = new_step;

    ucs_assert(params->map->ep_cnt == 1);

    new_step->pending_mask = 0;
    new_step->req_params   = params;
    new_step->num_req      = 1;
    new_step->reqs         = (ucp_coll_req_t*)(step + 1);
    new_step->del_op       = NULL;
    new_step->partial_data = NULL;
    new_step->step_cb_args = NULL;

    return ucp_coll_req_fill(params, new_step->reqs);
}

static inline ucs_status_t ucp_coll_direct_many_common(ucp_coll_req_params_t *params,
        ucp_coll_step_t **step)
{
    ucp_coll_topo_map_t *map = params->map;
    unsigned req_idx, req_count = map->ep_cnt;

    ucp_coll_step_t *new_step =
    		UCS_ALLOC_CHECK(sizeof(ucp_coll_step_t) + (sizeof(ucp_coll_req_t) * req_count), "step");

    new_step->num_req   = req_count;
    new_step->reqs      = (ucp_coll_req_t*)(step + 1);
    // TODO: params->internal_cb = ucp_coll_bitfield_complete_cb;

    /*
     * HACK: Array of pointers to this step,
     * to be used as context on request completion call-backs.
     */
    new_step->step_cb_args =
    		UCS_ALLOC_CHECK(sizeof(void*) * req_count, "step callbacks");
    for (req_idx = 0; req_idx < req_count; req_idx++) {
    	new_step->step_cb_args[req_idx] = new_step;
    }

    /*
     * Additional space for partial datatypes:
     * Since some of the collectives use "streaming" rather then "buffered"
     * receiving, if the packet exceeds MTU size the datatype can be cutoff
     * in the middle, preventing immediate use (e.g. go only 2 bytes out of 4).
     * The partial datatype will be stored in this array, and processed once
     * the rest arrives.
     */
    if (UCP_COLL_REQ_IS_REDUCE(params->flags)) {
        new_step->partial_data =
        		UCS_ALLOC_CHECK(params->coll_params->dsize * req_count, "partial datatypes");
    }

    *step = new_step;
    return ucp_coll_req_fill(params, new_step->reqs);
}

ucs_status_t ucp_coll_direct_many(ucp_coll_req_params_t *params,
        ucp_coll_step_t **step)
{
    return ucp_coll_direct_many_common(params, step);
}

ucs_status_t ucp_coll_direct_one_target(ucp_coll_req_params_t *params,
        ucp_coll_step_t **step)
{
    ucs_status_t error = ucp_coll_topo_generate_target_map(params->tree,
    		params->coll_params->root, &params->map);
    return (error != UCS_OK) ? error : ucp_coll_direct_one(params, step);
}

static inline ucs_status_t ucp_coll_direct_target_exchange(ucp_coll_req_params_t *params)
{
    ucs_status_t error = UCS_OK;
    ucp_group_rank_t root_idx;
    for (root_idx = 0;
         (root_idx < params->map->ep_cnt) && (error == UCS_OK);
         root_idx++) {
        //uct_ep_t *root_conn = &params->map->eps[root_idx];
        //ucp_coll_proc_ptr_t *root_proc = *root_conn;
        //error = ucp_coll_topo_get_ep(req_spec->peers, *root_proc,
        //        root_proc - req_spec->peers->procs, NULL, root_conn);
    }
    return error;
}

ucs_status_t ucp_coll_direct_many_target(ucp_coll_req_params_t *params,
        ucp_coll_step_t **step)
{
    ucs_status_t error = ucp_coll_direct_target_exchange(params);
    return (error != UCS_OK) ? error :
            ucp_coll_direct_many(params, step);
}
