/*
* Cplanyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#include "ucg_plan.h"
#include "ucg_group.h"
#include <ucp/api/ucpx.h> // Temporary, for PoC purposes

#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_worker.h>
#include <ucs/datastruct/queue.h>
#include <ucs/datastruct/list.h>
#include <ucs/profile/profile.h>
#include <ucs/debug/memtrack.h>
#include "../api/ucg_plan_component.h"

#define UCG_GROUP_COLLECTIVE_MODIFIER_MASK UCS_MASK(7)
#define UCP_OP_MASK(flags) \
    (flags & UCG_GROUP_COLLECTIVE_MODIFIER_MASK)

/*
 * The tag (64 bits) for UCT sends for collective operations is composed of
 * the group ID (determined in group creation time) and the operation ID
 * (the index of this operation on its group). Note that setting the latter is
 * part of the fast-path.
 */
typedef union ucg_req_tag {
    struct {
        ucg_group_id_t group_id;
        ucg_coll_id_t coll_id;
    };
    ucp_tag_t full; // TODO: check if there's a conflict with regular UCP tags
} ucg_req_tag_t;

struct ucp_group {
    /**
     * The operations cache contains completed past operations.
     * This hash-table is based on the first 7 modifier flags -
     * the ones determining the network pattern of the collective.
     */
    ucs_list_link_t cache[UCG_GROUP_COLLECTIVE_MODIFIER_MASK];

    ucg_worker_h             worker;       /* for conn. est. and progress calls */
    ucg_coll_id_t            next_id;      /* for the next collective operation */
    ucg_group_id_t           group_id;     /* part of the message tag */
    ucs_queue_head_t         outstanding;  /* operations currently executed */
    ucs_list_link_t          list;         /* worker's group list */
    const ucg_group_params_t params;       /* parameters, for future connections */

    ucg_plan_t              *plans[0];//UCG_TOPO_LAST];         /* plan information */
};

static inline ucs_status_t ucg_group_get_cached_plan(ucg_group_h group,
        ucg_collective_params_t *params,
        ucg_plan_t **instance_plan)
{
    ucs_list_link_t *cache_list = &group->cache[UCP_OP_MASK(params->flags)];
    if (ucs_list_is_empty(cache_list)) {
        return UCS_ERR_NO_ELEM;
    }

    /* TODO: restore!
    ucg_plan_t *plan;
    ucs_list_for_each(plan, cache_list, cache_list) {
        if (memcmp(&plan->params, params, sizeof(*params))) {
            ucg_plan_recycle(plan);
            return UCS_OK;
        }
    }
    */
    return UCS_ERR_NO_ELEM;
}

ucs_status_t ucg_group_create(ucg_worker_h worker,
        const ucg_group_params_t *params,
        ucg_group_h *group_p)
{
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucg_groups_t *ctx = (ucg_groups_t*)ucp_worker_get_groups_ctx(worker);

    /* allocate a new group, and fill in the fields */
    struct ucp_group *new_group = UCS_ALLOC_CHECK(sizeof(struct ucp_group), "communicator");
    new_group->group_id         = ctx->next_id++;
    new_group->worker           = worker;
    new_group->next_id          = 0;
    ucs_queue_head_init(&new_group->outstanding);
    memcpy((ucg_group_params_t*)&new_group->params, params, sizeof(*params));

    unsigned c_idx;
    for (c_idx = 0; c_idx < UCG_GROUP_COLLECTIVE_MODIFIER_MASK; c_idx++) {
        ucs_list_head_init(&new_group->cache[c_idx]);
    }

    ucs_list_add_head(&ctx->head, &new_group->list);
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    *group_p = new_group;
    return UCS_OK;
}

void ucg_group_destroy(ucg_group_h group)
{
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(group->worker);

    ucs_list_del(&group->list);

    /* TODO:
    while(!ucs_queue_is_empty(&group->outstanding)) {
        ucg_plan_destroy(ucs_queue_pull_elem_non_empty(&group->outstanding,
                ucg_plan_t, queue));
    }

    int i;
    for (i = 0; i < UCG_GROUP_COLLECTIVE_MODIFIER_MASK; i++) {
        ucg_plan_t *plan, *tmp;
        ucs_list_for_each_safe(plan, tmp, &group->cache[i], cache_list) {
            ucg_plan_destroy(plan);
        }
    }

    enum ucg_tplano_type type;
    for (type = 0; type < UCG_TOPO_LAST; type++) {
        ucg_tplano_destroy(group->tplano[type]);
    }*/
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(group->worker);
}

UCS_PROFILE_FUNC(ucs_status_t, ucg_collective_create,
        (group, params, coll), ucg_group_h group,
        ucg_collective_params_t *params, ucg_coll_h *coll)
{
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(group->worker);

    /* Check the recycling/cache for this collective */
    ucg_plan_t **ret_plan = (ucg_plan_t**)coll;
    ucs_status_t ret = ucg_group_get_cached_plan(group, params, ret_plan);
    if (ret != UCS_ERR_NO_ELEM) {
        goto out;
    }

    /* Select which plan to use for this collective operation */
    ucg_plan_component_t *planc;
    ret = ucg_plan_select_component(NULL, 0, &group->params, params, &planc);
    if (ret != UCS_OK) {
        goto out;
    }

    ret = ucg_plan(planc, &group->params, params, ret_plan); //TODO: pass group->worker,group->group_id++ ?
    if (ret != UCS_OK) {
        goto out;
    }

    // TODO: (*ret_plan)->group = group;
out:
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(group->worker);
    return ret;
}

UCS_PROFILE_FUNC(ucs_status_ptr_t, ucg_collective_start_nb,
                 (coll), ucg_coll_h coll)
{
    ucs_status_ptr_t ret;
    ucg_request_t *req = NULL;
    ucg_plan_t *plan = (ucg_plan_t*)coll;
    ucg_worker_h worker = plan->worker;

    /* Since group was created - don't need UCP_CONTEXT_CHECK_FEATURE_FLAGS */
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucs_trace_req("coll_start_nb coll %p", coll);

    ret = (ucs_status_ptr_t)ucg_collective_req_init(plan, worker, plan->params.comp_cb, (void**)&req);
    if (ucs_likely(ret == UCS_OK)) {
        /* Generate the next tag to be used for messages */
        // TODO: ucg_collective_update_tags(plan);

        /* Start the first step of the collective operation */
        ret = UCS_STATUS_PTR(ucg_step_execute(&plan->steps[0]));
        if (ucs_likely(ret == UCS_OK)) {
            // TODO: make sure UCS_OK and UCS_INPROGRESS work correctly!
            ret = plan->cb_req = req;
        } else {
            // TODO: ucp_request_put(req);
        }
    }

    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(plan->group->worker);
    return ret;
}

UCS_PROFILE_FUNC(ucs_status_t, ucg_collective_start_nbr,
                 (coll, request), ucg_coll_h coll, void *request)
{
    /* Fill in UCP request details */
    ucg_plan_t *plan    = (ucg_plan_t*)coll;
    ucg_worker_h worker = plan->worker;

    /* Since group was created - don't need UCP_CONTEXT_CHECK_FEATURE_FLAGS */
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucs_status_t ret;
    if (plan->cb_req) {
        ucs_error("Only one instance of a persistent collective operation at a time is supported.");
        ret = UCS_ERR_UNSUPPORTED;
    } else {
        ret = ucg_collective_req_init(plan, worker, plan->params.comp_cb, (void**)&request);
        if (ucs_likely(ret == UCS_OK)) {
            ucs_trace_req("coll_start_nbr coll %p req %p", coll, request);

            /* Mark this operation as "in-use", and set the callback argument */
            plan->cb_req = request;

            /* Generate the next tag to be used for messages */
            ucg_collective_update_tags(plan);

            /* Start the first step of the collective operation */
            ret = ucg_step_execute(&plan->steps[0]);

            /* Add to group queue */
            ucs_queue_push(&plan->group->outstanding, &plan->queue);

            /* Add to statistics */
            // TODO: UCS_STATS_UPDATE_COUNTER(plan->group, plan->params.flags, 1);
        }
    }

    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    return ret;
}

ucs_status_t ucg_collective_destroy(ucg_coll_h coll)
{
    return ucg_plan_recycle((ucg_plan_t*)coll);
}

ucs_status_t ucg_worker_groups_init(void **groups_ctx)
{
    ucg_groups_t *ctx = ucs_malloc(sizeof(ucg_groups_t), "ucc groups context");
    if (ctx == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    ctx->next_id = 0;
    ucs_list_head_init(&ctx->head);
    *groups_ctx = ctx;
    return UCS_OK;
}

void ucg_worker_groups_cleanup(void *groups_ctx)
{
    ucg_group_h group, tmp;
    ucs_list_for_each_safe(group, tmp, &((ucg_groups_t*)groups_ctx)->head, list) {
        ucg_group_destroy(group);
    }
}

void ucg_group_recycle_plan(ucg_group_h group, ucg_plan_t *plan)
{
    ucs_list_link_t *cache_list = &group->cache[UCP_OP_MASK(plan->params.flags)];
    ucs_list_add_head(cache_list, &plan->cache_list);
    ucg_plan_recycle(plan);
}

ucs_status_t ucg_plan_connect(ucg_group_h group, ucg_group_member_index_t idx, ucp_ep_h *ep_p)
{
    /* fill-in UCP connection parameters */
    ucp_address_t *remote_addr;
    ucs_status_t status = group->params.resolve_address_f(group->params.cb_group_obj, idx, &remote_addr);
    if (status != UCS_OK) {
        ucs_error("failed to obtain a UCP endpoint from the external callback");
    }

    ucp_ep_params_t ep_params = {
            .field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS,
            .address = remote_addr
    };

    status = ucp_ep_create(group->worker, &ep_params, ep_p);
    group->params.release_address_f(remote_addr);
    return status;
}

