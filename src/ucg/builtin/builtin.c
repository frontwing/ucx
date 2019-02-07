/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include "builtin.h"

#include <string.h>
#include <ucs/debug/memtrack.h>

static ucs_config_field_t ucg_builtin_topo_config_table[] = {
    {"", "", NULL, ucs_offsetof(ucg_builtin_config_t, super), UCS_CONFIG_TYPE_TABLE(ucg_topo_config_table)},

    {NULL}
};

static ucs_status_t ucg_builtin_query(ucg_plan_desc_t **desc_p, unsigned *num_descs_p)
{
    return ucg_plan_single_resource(&ucg_builtin_component, desc_p, num_descs_p);
}

static enum ucg_topo_type ucg_builtin_choose_type(enum ucg_collective_modifiers flags)
{
    if (flags & UCG_GROUP_COLLECTIVE_MODIFIER_SINGLE_SOURCE) {
        return UCG_TOPO_TREE_FANOUT;
    }

    if (flags & UCG_GROUP_COLLECTIVE_MODIFIER_SINGLE_DESTINATION) {
        return UCG_TOPO_TREE_FANIN;
    }

    if (flags & UCG_GROUP_COLLECTIVE_MODIFIER_AGGREGATE) {
        return UCG_TOPO_RECURSIVE;
    }

    return UCG_TOPO_TREE_FANIN_FANOUT;
}

static ucs_status_t ucg_builtin_plan(ucg_plan_component_t *planc,
                                     const ucg_group_params_t *group_params,
                                     const ucg_collective_params_t *coll_params,
                                     ucp_request_t ***plan_p)
{
    ucg_topo_t *topo;
    switch(ucg_builtin_choose_type(coll_params->flags)) {
    case UCG_TOPO_RECURSIVE:
        ret = ucg_builtin_recursive_create(group_params, coll_params, &topo);

    default:
        ret = ucg_builtin_tree_create(group_params, coll_params, &topo);
    }


    mpi_reduce = group_params->mpi_reduce_f; // TODO: move, to optimize

    return ucg_op_create(topo, plan_p);
}

static ucs_status_t ucg_builtin_launch_nb(ucg_plan_t *plan, ucg_coll_id_t coll_id, ucg_request_t **req)
{
    ucg_op_t *op = (ucg_op_t*)&plan->priv;
    ucg_step_execute(&op->steps[0]);
}

static ucs_status_t ucg_builtin_launch_nbr(ucg_plan_t *plan, ucg_coll_id_t coll_id, ucg_request_t *req)
{
    ucg_op_t *op = (ucg_op_t*)&plan->priv;
    ucg_step_execute(&op->steps[0]);
}

static void ucg_builtin_destroy(ucg_plan_t *plan)
{
    ucg_op_t *op = (ucg_op_t*)&plan->priv;
    ucg_op_destroy(op);
}

UCG_PLAN_COMPONENT_DEFINE(ucg_builtin_component, UCG_BUILTIN_PLAN_NAME,
                          ucg_builtin, ucg_builtin_plan, ucg_builtin_launch_nb,
                          ucg_builtin_launch_nbr, ucg_builtin_destroy,
                          "BUILTIN_", ucg_builtin_topo_config_table,
                          ucg_builtin_topo_config_t);

