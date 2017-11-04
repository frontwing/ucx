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

#include "group.h"

#include <ucp/core/ucp_worker.h>

ucs_status_t ucp_coll_init(ucp_coll_ctx_t **ctx)
{
	*ctx = UCS_ALLOC_CHECK(sizeof(**ctx), "ucp_collective_context");
	ucs_list_head_init(&(*ctx)->groups);
	(*ctx)->next_group_id = 0;
	return UCS_OK;
}

void ucp_coll_finalize(ucp_coll_ctx_t *ctx)
{
	ucs_free(ctx);
}

ucs_status_t ucp_coll_group_create(ucp_worker_t *worker,
		                           const ucp_group_create_params_t *params,
								   ucp_coll_group_t **group)
{
    enum ucp_group_collective_type coll_idx;
    ucp_coll_group_t *new_group =
    		UCS_ALLOC_CHECK(sizeof(**group), "communicator");

    // TODO: Convert from params->mpi_procs to group->peers (ep/iface)
    // TODO: generate a multi-tree, and tag them "context->next_group_id++"
    ucs_status_t error = ucp_coll_topo_tree_create(params, &new_group->tree);
    if (error != UCS_OK) {
    	ucs_free(new_group);
        return error;
    }

    for (coll_idx = 0; coll_idx < UCP_GROUP_COLLECTIVE_TYPE_LAST; coll_idx++) {
        ucs_list_head_init(&new_group->op_lru_cache[coll_idx]);
    }

    ucs_list_head_init(&new_group->ops);
    new_group->group_id = worker->coll_ctx->next_group_id++; // TODO: mutex
    new_group->is_barrier_outstanding = 0;
    new_group->next_coll_id = 0;
    *group = new_group;
    return UCS_OK;
}

void ucp_coll_group_destroy(ucp_coll_group_t *group)
{
    enum ucp_group_collective_type coll_idx;
    while(!ucs_list_is_empty(&group->ops)) {
    	ucp_coll_op_destroy(ucs_list_extract_head(&group->ops,
    			ucp_coll_op_t, queue));
    }

    for (coll_idx = 0; coll_idx < UCP_GROUP_COLLECTIVE_TYPE_LAST; coll_idx++) {
    	while (!ucs_list_is_empty(&group->op_lru_cache[coll_idx])) {
    		ucp_coll_op_destroy(ucs_list_extract_head(&group->op_lru_cache[coll_idx],
    				ucp_coll_op_t, cache));
    	}
    }

    ucp_coll_topo_tree_destroy(group->tree);
}

ucs_status_t ucp_coll_op_create(ucp_coll_group_t *group,
											  ucp_group_collective_params_t *params,
											  ucp_coll_op_t **coll_op)
{
    ucs_status_t error;

    /* fill in request specification */
    ucp_coll_req_params_t req_params = {
    		.tag = {
    			.group_id = group->group_id,
    			.coll_id  = group->next_coll_id++, // TODO: mutex
    		}
    };

    /* look for a matching cached operation */
    error = ucp_coll_op_get(params, &group->op_lru_cache[params->type], coll_op);
    if (error != UCS_ERR_NO_ELEM) {
        return error; /* also if a cached op was launched successfully */
    }

    /* no dice, create a new operation */
    error = ucp_coll_op_new(group->tree, params, &req_params, group, coll_op);
    return error;
}

ucs_status_ptr_t ucp_coll_op_start(ucp_coll_op_t *coll_op)
{
    int can_launch_op = !coll_op->group->is_barrier_outstanding;
    if (ucs_unlikely(coll_op->coll_params.type == UCP_GROUP_COLLECTIVE_TYPE_BARRIER)) {
    	coll_op->group->is_barrier_outstanding = 1;
    }
    ucs_list_insert_before(&coll_op->group->ops, &coll_op->queue);
    return can_launch_op ? ucp_coll_op_run(coll_op) : UCS_STATUS_PTR(UCS_INPROGRESS);
}

void ucp_coll_op_destroy(ucp_coll_op_t *coll_op)
{
	//TODO: put op back into the hash-table?
	ucp_coll_op_del(coll_op);
}

void ucp_coll_group_barrier_op_cb(ucp_coll_group_t *group, ucp_coll_op_t *op) {
    //ucp_coll_step_t *step;
    //ucp_coll_op_t *next_op = NULL;
    //enum ucp_group_collective_type op_coll = op->coll_params.type;

    // delete all items in the group-queue up to this op (excluded)
    ucs_assert(!ucs_list_is_empty(&group->ops)); /* TODO: return:
    while ((!ucs_list_is_empty(&group->ops)) &&
           ((next_op = ucs_queue_pull_elem_non_empty(&group->ops,
                   ucp_coll_op_t, queue)) != op)) {
        ucs_queue_push_head(&group->op_lru_cache[next_op->coll_params.type], &next_op->queue);
        next_op = NULL;
    }
    if (next_op) {
        ucs_queue_push_head(&group->ops, &next_op->queue);
    }

    if (ucs_unlikely(op_coll == UCP_GROUP_COLLECTIVE_TYPE_BARRIER)) {
        // launch all ops up to next barrier(/ibarrier)
    	ucp_coll_step_t ste
        ucs_queue_iter_t iter =
                ucs_queue_iter_next(ucs_queue_iter_begin(&group->ops));
        if (!ucs_queue_iter_end(&group->ops, iter)) {
            iter = ucs_queue_iter_next(iter);
            while (!ucs_queue_iter_end(&group->ops, iter)) {
                next_op = ucs_queue_iter_elem(next_op, &iter, queue);
                op_coll = next_op->coll_params.type;

                if (ucs_unlikely(op_coll == UCP_GROUP_COLLECTIVE_TYPE_BARRIER)) {
                    break;
                }

                ucs_assert(!ucs_queue_is_empty(&next_op->steps));
                ucp_coll_step_launch(ucs_queue_head_elem_non_empty(&next_op->steps,
                        ucp_coll_step_t, queue));
                iter = ucs_queue_iter_next(iter);
            }
        }

        if (ucs_queue_iter_end(&group->ops, iter)) {
            group->is_barrier_outstanding = 0;
        }
    }
    */
}
