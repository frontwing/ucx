/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifndef UCG_PLAN_H_
#define UCG_PLAN_H_

#include "../api/ucg_plan_component.h"

/* Functions on all planning components */

ucs_status_t ucg_plan_query(ucg_plan_desc_t **resources_p, unsigned *num_resources_p);
void ucg_plan_release_list(ucg_plan_desc_t *resources);
ucs_status_t ucg_plan_select_component(ucg_plan_desc_t *planners,
                                       unsigned *num_planners,
                                       const ucg_group_params_t *group_params,
                                       const ucg_collective_params_t *coll_params,
                                       ucg_plan_component_t **planc);

/* Functions on a specific component */
#define ucg_plan(planc, group_params, coll_params, plan_p) \
    (planc->plan(planc, group_params, coll_params, plan_p))

#define ucg_plan_launch_nb(planc, plan, coll_id, req) \
    (planc->launch_nb(plan, coll_id, req))

#define ucg_plan_launch_nbr(planc, plan, coll_id, req) \
    (planc->launch_nbr(plan, coll_id, req));

#define ucg_plan_destroy(planc, plan) \
    (planc->destroy(plan))

#endif /* UCG_TOPO_H_ */
