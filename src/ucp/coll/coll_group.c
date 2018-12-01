#include "coll_ops.h"
#include "coll_group.h"

#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_worker.h>
#include <ucp/core/ucp_request.inl> /* for ucp_request_get() */
#include <ucs/datastruct/queue.h>
#include <ucs/datastruct/list.h>
#include <ucs/profile/profile.h>
#include <ucs/debug/memtrack.h>

#define UCP_GROUP_COLLECTIVE_MODIFIER_MASK UCS_MASK(7)
#define UCP_OP_MASK(flags) \
	(flags & UCP_GROUP_COLLECTIVE_MODIFIER_MASK)

/*
 * The tag (64 bits) for UCT sends for collective operations is composed of
 * the group ID (determined in group creation time) and the operation ID
 * (the index of this operation on its group). Note that setting the latter is
 * part of the fast-path.
 */
typedef union ucp_coll_req_tag {
    struct {
    	ucp_coll_group_id_t group_id;
    	ucp_coll_op_id_t coll_id;
    };
    ucp_tag_t full; // TODO: check if there's a conflict with regular UCP tags
} ucp_coll_req_tag_t;

struct ucp_group {
	/**
	 * The operations cache contains completed past operations.
	 * This hash-table is based on the first 7 modifier flags -
	 * the ones determining the network pattern of the collective.
	 */
	ucs_list_link_t cache[UCP_GROUP_COLLECTIVE_MODIFIER_MASK];

	ucp_worker_h             worker;       /* for conn. est. and progress calls */
	ucp_coll_op_id_t         next_id;      /* for the next collective operation */
	ucp_coll_group_id_t      group_id;     /* part of the message tag */
	ucs_queue_head_t         outstanding;  /* operations currently executed */
	ucs_list_link_t          list;         /* worker's group list */
	const ucp_group_params_t params;       /* parameters, for future connections */

	ucp_coll_topo_t         *topo[UCP_COLL_TOPO_LAST];         /* topology information */
};

static inline ucs_status_t ucp_coll_group_get_cached_op(ucp_group_h group,
		ucp_group_collective_params_t *params,
		ucp_coll_op_t **instance_op)
{
	ucs_list_link_t *cache_list = &group->cache[UCP_OP_MASK(params->flags)];
	if (ucs_list_is_empty(cache_list)) {
		return UCS_ERR_NO_ELEM;
	}

	ucp_coll_op_t *op;
	ucs_list_for_each(op, cache_list, cache_list) {
		if (memcmp(&op->params, params, sizeof(*params))) {
			ucp_coll_op_recycle(op);
			return UCS_OK;
		}
	}

	return UCS_ERR_NO_ELEM;
}

ucs_status_t ucp_group_create(ucp_worker_h worker,
                              const ucp_group_params_t *params,
                              ucp_group_h *group_p)
{
    UCP_CONTEXT_CHECK_FEATURE_FLAGS(worker->context, UCP_FEATURE_COLL,
                                    return UCS_ERR_INVALID_PARAM);
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    /* allocate a new group, and fill in the fields */
    struct ucp_group *new_group = UCS_ALLOC_CHECK(sizeof(struct ucp_group), "communicator");
    mpi_reduce                  = params->mpi_reduce_f;
    new_group->group_id         = worker->groups.next_id++;
    new_group->worker           = worker;
    new_group->next_id          = 0;
    ucs_queue_head_init(&new_group->outstanding);
    memcpy((ucp_group_params_t*)&new_group->params, params, sizeof(*params));

    unsigned c_idx;
    for (c_idx = 0; c_idx < UCP_GROUP_COLLECTIVE_MODIFIER_MASK; c_idx++) {
    	ucs_list_head_init(&new_group->cache[c_idx]);
    }

    /* prepare the topologies for collectives on this group */
	ucs_status_t status;
    enum ucp_coll_topo_type type;
	struct ucp_coll_topo_params topo_params = {
			.group_params     = params,
			.group            = new_group
	};

    for (type = 0; ((type < UCP_COLL_TOPO_LAST) && (status == UCS_OK)); type++) {
    	/* Set type-specific parameters */
    	//TODO: use actual topo params (from config?)
    	topo_params.type = type;
    	if (type == UCP_COLL_TOPO_RECURSIVE) {
    		topo_params.recursive_factor = 2;
    	} else {
    		topo_params.tree_radix = 7;
    	}

    	status = ucp_coll_topo_create(&topo_params, &new_group->topo[type]);
    }

	if (ucs_unlikely(status != UCS_OK)) {
		while (type) {
			ucp_coll_topo_destroy(new_group->topo[--type]);
		}
		ucs_free(new_group);
	} else {
		ucs_list_add_head(&worker->groups.head, &new_group->list);
	}

	UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    *group_p = new_group;
    return status;
}

void ucp_group_destroy(ucp_group_h group)
{
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(group->worker);

    ucs_list_del(&group->list);

    while(!ucs_queue_is_empty(&group->outstanding)) {
    	ucp_coll_op_destroy(ucs_queue_pull_elem_non_empty(&group->outstanding,
    			ucp_coll_op_t, queue));
    }

    int i;
    for (i = 0; i < UCP_GROUP_COLLECTIVE_MODIFIER_MASK; i++) {
    	ucp_coll_op_t *op, *tmp;
    	ucs_list_for_each_safe(op, tmp, &group->cache[i], cache_list) {
    		ucp_coll_op_destroy(op);
    	}
    }

    enum ucp_coll_topo_type type;
    for (type = 0; type < UCP_COLL_TOPO_LAST; type++) {
    	ucp_coll_topo_destroy(group->topo[type]);
    }
	UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(group->worker);
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_group_collective_create,
		(group, params, coll), ucp_group_h group,
		ucp_group_collective_params_t *params, ucp_coll_h *coll)
{
	UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(group->worker);

    /* Check the recycling/cache for this collective */
    ucp_coll_op_t **ret_op = (ucp_coll_op_t**)coll;
    ucs_status_t ret = ucp_coll_group_get_cached_op(group, params, ret_op);
    if (ret != UCS_ERR_NO_ELEM) {
    	goto out;
    }

    /* Create a new collective operation */
    enum ucp_coll_topo_type type = ucp_coll_topo_choose_type(params->flags);
    ret = ucp_coll_op_create(group->worker, group->topo[type], group->group_id++, params, ret_op);
    if (ret != UCS_OK) {
    	goto out;
    }

    (*ret_op)->group = group;
out:
	UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(group->worker);
    return ret;
}

static UCS_F_ALWAYS_INLINE void
ucp_group_collective_req_init(ucp_request_t* req, ucp_coll_op_t *op, ucp_worker_h worker)
{
    req->flags             = UCP_REQUEST_FLAG_COLLECTIVE;
    req->collective.op     = op;
    req->collective.worker = worker;

    ucp_group_collective_callback_t cb = op->params.comp_cb;
    if (cb != NULL) {
    	op->cb_req = req + 1;
    	ucp_request_set_callback(req, collective.comp_cb, cb);
    }
}

static UCS_F_ALWAYS_INLINE void
ucp_group_collective_update_tags(ucp_coll_op_t *op) {
	ucp_group_h group = op->group;
	ucp_coll_req_tag_t tag = {
			.group_id = group->group_id,
			.coll_id  = group->next_id++
	};

	unsigned step_idx = 0;
	ucp_coll_step_t *step = &op->steps[0];
	for (step_idx = 0; step_idx < op->step_cnt; step_idx++, step++) {
		ucs_queue_push(&op->step_q, &step->queue);
		ucp_coll_step_set_tag(step, tag.full);
	}
}

UCS_PROFILE_FUNC(ucs_status_ptr_t, ucp_group_collective_start_nb,
                 (coll), ucp_coll_h coll)
{
	ucs_status_ptr_t ret;
	ucp_coll_op_t *op   = (ucp_coll_op_t*)coll;
	ucp_worker_h worker = op->worker;

	/* Since group was created - don't need UCP_CONTEXT_CHECK_FEATURE_FLAGS */
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucs_trace_req("coll_start_nb coll %p", coll);

    ucp_request_t *req = ucp_request_get(worker);
    if (ucs_likely(req != NULL)) {
    	ucp_group_collective_req_init(req, op, worker);

        /* Generate the next tag to be used for messages */
    	ucp_group_collective_update_tags(op);

    	/* Start the first step of the collective operation */
    	ret = UCS_STATUS_PTR(ucp_coll_step_execute(&op->steps[0]));
    	if (ucs_likely(ret == UCS_OK)) {
    		// TODO: make sure UCS_OK and UCS_INPROGRESS work correctly!
    		ret = op->cb_req = req + 1;
    	} else {
    		ucp_request_put(req);
    	}
    } else {
        ret = UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);
    }

    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(op->group->worker);
    return ret;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_group_collective_start_nbr,
                 (coll, request), ucp_coll_h coll, void *request)
{
	/* Fill in UCP request details */
    ucp_request_t *req     = (ucp_request_t *)request - 1;
	ucp_coll_op_t *op      = (ucp_coll_op_t*)coll;
	ucp_worker_h worker    = op->worker;

    /* Since group was created - don't need UCP_CONTEXT_CHECK_FEATURE_FLAGS */
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucs_trace_req("coll_start_nbr coll %p req %p", coll, request);

	ucs_status_t ret;
    if (op->cb_req) {
		ucs_error("Only one instance of a persistent collective operation at a time is supported.");
    	ret = UCS_ERR_UNSUPPORTED;
    } else {
    	ucp_group_collective_req_init(req, op, worker);

    	/* Mark this operation as "in-use", and set the callback argument */
    	op->cb_req = request;

    	/* Generate the next tag to be used for messages */
    	ucp_group_collective_update_tags(op);

    	/* Start the first step of the collective operation */
    	ret = ucp_coll_step_execute(&op->steps[0]);

    	/* Add to group queue */
    	ucs_queue_push(&op->group->outstanding, &op->queue);

    	/* Add to statistics */
    	// TODO: UCS_STATS_UPDATE_COUNTER(op->group, op->params.flags, 1);
    }

    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    return ret;
}

ucs_status_t ucp_group_collective_destroy(ucp_coll_h coll)
{
	return ucp_coll_op_recycle((ucp_coll_op_t*)coll);
}

ucs_status_t ucp_worker_groups_init(ucp_groups_t *groups_ctx)
{
	groups_ctx->next_id = 0;
	ucs_list_head_init(&groups_ctx->head);
	return UCS_OK;
}

void ucp_worker_groups_cleanup(ucp_groups_t *groups_ctx)
{
	ucp_group_h group, tmp;
    ucs_list_for_each_safe(group, tmp, &groups_ctx->head, list) {
		ucp_group_destroy(group);
	}
}

void ucp_coll_group_recycle_op(ucp_group_h group, ucp_coll_op_t *op)
{
	ucs_list_link_t *cache_list = &group->cache[UCP_OP_MASK(op->params.flags)];
	ucs_list_add_head(cache_list, &op->cache_list);
	ucp_coll_op_recycle(op);
}

ucs_status_t ucp_coll_topo_connect(ucp_group_h group, ucp_group_rank_t rank, ucp_ep_h *ep_p)
{
	/* Sanity checks */
	ucs_assert(rank != group->params.my_rank_index);

	/* fill-in UCP connection parameters */
	ucs_status_t status = group->params.mpi_get_ep_f(group->params.cb_group_obj, rank, ep_p);
	if (status != UCS_OK) {
		ucs_error("failed to obtain a UCP endpoint from the external callback");
	}
	return status;
}
