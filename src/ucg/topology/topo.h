/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifndef UCG_TOPO_H_
#define UCG_TOPO_H_

#include <ucs/datastruct/list_types.h>
#include <ucg/api/ucg.h>

#define UCG_MASK         (UCG_GROUP_COLLECTIVE_MODIFIER_AGGREGATE_STABLE - 1)
#define UCG_GET_EPS(phs) ((phs)->ep_cnt == 1 ? &(phs)->single_ep : (phs)->multi_eps) // TODO: apply/use

enum ucg_topo_type {
    UCG_TOPO_TREE_FANIN = 0,
    UCG_TOPO_TREE_FANOUT,
    UCG_TOPO_TREE_FANIN_FANOUT,
    UCG_TOPO_RECURSIVE,
    UCG_TOPO_ALLTOALL_AGGREGATION,
    UCG_TOPO_ALLTOALL_BRCUK,
    UCG_TOPO_LAST
};

typedef struct ucg_topo_params {
    const ucg_group_params_t *group_params; /* Original group parameters for UCP */
    ucg_group_h               group;        /* for connection establishment */
    enum ucg_topo_type        type;
    union {
        struct {
            unsigned          tree_radix;
            unsigned          tree_roots;
        };
        unsigned              recursive_factor; /* e.g. 2 for recursive doubling */
        struct {
            unsigned          neighbor_dimension; /* e.g. 2 for a 2D grid */
            int               enable_wrapping;
        };
    };
} ucg_topo_params_t;

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
    enum ucg_topo_method_type method;     /* how to apply this map */
} ucg_topo_phase_t;

typedef struct ucg_topo {
    unsigned         phs_cnt; /* phase count */
    unsigned         ep_cnt;  /* total endpoint count */
    ucg_topo_phase_t phss[];  /* topology's phases */
/*  ucp_ep_h         eps[];    * logically located here */
} ucg_topo_t;

ucs_status_t ucg_topo_create(ucg_topo_params_t *params, ucg_topo_t **topo_p);

void ucg_topo_destroy(ucg_topo_t *topo);

ucs_status_t ucg_topo_connect(ucg_group_h group, ucg_group_member_index_t idx, ucp_ep_h *ep_p);

static UCS_F_ALWAYS_INLINE
enum ucg_topo_type ucg_topo_choose_type(enum ucg_collective_modifiers flags)
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


/*
 * Per-topology prototypes
 */

ucs_status_t ucg_topo_recursive_create(struct ucg_topo_params *params, struct ucg_topo **topo_p);

ucs_status_t ucg_topo_tree_create(struct ucg_topo_params *params, struct ucg_topo **topo_p);

ucs_status_t ucg_topo_tree_set_root(struct ucg_topo *tree_topo, ucg_group_member_index_t root, struct ucg_topo **topo_p);

#endif /* UCG_TOPO_H_ */
