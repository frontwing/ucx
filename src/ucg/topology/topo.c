/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#include "topo.h"

#include <ucs/debug/memtrack.h>


ucs_status_t ucg_topo_create(struct ucg_topo_params *params,
        ucg_topo_t **topo_p)
{
    switch(params->type) {
    case UCG_TOPO_RECURSIVE:
        return ucg_topo_recursive_create(params, topo_p);

    default:
        return ucg_topo_tree_create(params, topo_p);
    }

    return UCS_ERR_UNSUPPORTED;
}

void ucg_topo_destroy(struct ucg_topo *topo)
{
    unsigned phs_idx, ep_idx;
    ucg_topo_phase_t *phase = &topo->phss[0];
    for (phs_idx = 0; phs_idx < topo->phs_cnt; phs_idx++, phase++) {
        ucp_ep_h *eps = UCG_GET_EPS(phase);
        for (ep_idx = 0; ep_idx < phase->ep_cnt; ep_idx++, eps++) {
            ucp_ep_destroy(*eps);
        }
    }
    ucs_free(topo);
}
