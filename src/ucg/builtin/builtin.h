/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCG_BUILTIN_H
#define UCG_BUILTIN_H

#include "../api/ucg_plan_component.h"

#define UCG_BUILTIN_PLAN_NAME "builtin"

extern ucg_plan_component_t ucg_builtin_component;

/**
 * @brief Built-in topology descriptor
 */
typedef struct ucg_builtin_topo {
    struct ucg_topo super;   /**< Topology info */
} ucg_builtin_topo_t;

/**
 * Built-in topologies configuration.
 */
typedef struct ucg_builtin_config {
    ucg_topo_config_t super;
} ucg_builtin_config_t;

enum ucg_topo_type {
    UCG_TOPO_TREE_FANIN = 0,
    UCG_TOPO_TREE_FANOUT,
    UCG_TOPO_TREE_FANIN_FANOUT,
    UCG_TOPO_RECURSIVE,
    UCG_TOPO_ALLTOALL_AGGREGATION,
    UCG_TOPO_ALLTOALL_BRCUK,
    UCG_TOPO_LAST
};

enum ucg_topo_method_type {
    UCG_TOPO_METHOD_SEND, /* send to all peers in the map */
    UCG_TOPO_METHOD_RECV, /* recv from all peers in the map */
    UCG_TOPO_METHOD_REDUCE, /* recv and reduce from each peer */
    UCG_TOPO_METHOD_STABLE_REDUCE, /* recv and reduce in strict order */
    UCG_TOPO_METHOD_REDUCE_RECURSIVE, /* send+recv and reduce (RD) */
    UCG_TOPO_METHOD_STABLE_REDUCE_RECURSIVE, /* stable RD variant */
    UCG_TOPO_METHOD_NEIGHBOR, /* "exchange", for neighbor collectives */
};

typedef struct ucg_topo_phase {
    union {
        ucp_ep_h    *multi_eps;
        ucp_ep_h     single_ep;
    };
    unsigned         ep_cnt;
    enum ucg_topo_method_type method; /* how to apply this map */
} ucg_topo_phase_t;

typedef struct ucg_topo {
    unsigned              phs_cnt; /* phase count */
    unsigned              ep_cnt;  /* total endpoint count */
    ucg_topo_phase_t      phss[];  /* topology's phases */
/*  ucp_ep_h              eps[];    * logically located here */
} ucg_topo_t;


// ALEX TODO: decide which topologies are created at first, and what are the params...
ucs_status_t ucg_builtin_tree_create(const ucg_group_params_t *group_params,
                                     const ucg_collective_params_t *coll_params,
                                     struct ucg_topo **topo_p);
ucs_status_t ucg_topo_tree_set_root(struct ucg_topo *tree_topo, ucg_group_member_index_t root, struct ucg_topo **topo_p);
ucs_status_t ucg_builtin_recursive_create(const ucg_group_params_t *group_params,
                                          const ucg_collective_params_t *coll_params,
                                          ucg_topo_t **topo_p);
ucs_status_t ucg_topo_neighbor_create(const ucg_group_params_t *group_params,
                                      const ucg_collective_params_t *coll_params,
                                      ucg_topo_t **topo_p);

#endif
