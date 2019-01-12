#ifndef UCP_COLL_TOPO_H_
#define UCP_COLL_TOPO_H_

#include <uct/api/uct.h>
#include <ucp/api/ucp.h>
#include <ucs/datastruct/list_types.h>

#define UCP_COLL_MASK           (UCP_GROUP_COLLECTIVE_MODIFIER_AGGREGATE_STABLE - 1)
#define UCP_COLL_GET_EPS(phs)   ((phs)->ep_cnt == 1 ? &(phs)->single_ep : (phs)->multi_eps) // TODO: apply/use

enum ucp_coll_topo_type {
    UCP_COLL_TOPO_TREE_FANIN = 0,
    UCP_COLL_TOPO_TREE_FANOUT,
    UCP_COLL_TOPO_TREE_FANIN_FANOUT,
    UCP_COLL_TOPO_RECURSIVE,
    UCP_COLL_TOPO_ALLTOALL_AGGREGATION,
    UCP_COLL_TOPO_ALLTOALL_BRCUK,
    UCP_COLL_TOPO_LAST
};

typedef struct ucp_coll_topo_params {
    const ucp_group_params_t *group_params; /* Original group parameters for UCP */
    ucp_group_h               group;        /* for connection establishment */
    enum ucp_coll_topo_type   type;
    union {
        struct {
            unsigned tree_radix;
            unsigned tree_roots;
        };
        unsigned recursive_factor; /* e.g. 2 for recursive doubling */
        struct {
            unsigned neighbor_dimension; /* e.g. 2 for a 2D grid */
            int enable_wrapping;
        };
    };
} ucp_coll_topo_params_t;

enum ucp_coll_topo_method_type {
    UCP_COLL_TOPO_METHOD_SEND, /* send to all peers in the map */
    UCP_COLL_TOPO_METHOD_RECV, /* recv from all peers in the map */
    UCP_COLL_TOPO_METHOD_REDUCE, /* recv and reduce from each peer */
    UCP_COLL_TOPO_METHOD_STABLE_REDUCE, /* recv and reduce in strict order */
    UCP_COLL_TOPO_METHOD_REDUCE_RECURSIVE, /* send+recv and reduce (RD) */
    UCP_COLL_TOPO_METHOD_STABLE_REDUCE_RECURSIVE, /* stable RD variant */
    UCP_COLL_TOPO_METHOD_NEIGHBOR, /* "exchange", for neighbor collectives */
};

typedef struct ucp_coll_topo_phase {
    union {
        ucp_ep_h                  *multi_eps;
        ucp_ep_h                   single_ep;
    };
    unsigned                       ep_cnt;
    enum ucp_coll_topo_method_type method;     /* how to apply this map */
} ucp_coll_topo_phase_t;

typedef struct ucp_coll_topo {
    unsigned                           phs_cnt; /* phase count */
    unsigned                           ep_cnt;  /* total endpoint count */
    ucp_coll_topo_phase_t              phss[];  /* topology's phases */
/*  ucp_ep_h                           eps[];    * logically located here */
} ucp_coll_topo_t;

ucs_status_t ucp_coll_topo_create(ucp_coll_topo_params_t *params, ucp_coll_topo_t **topo_p);

void ucp_coll_topo_destroy(ucp_coll_topo_t *topo);

ucs_status_t ucp_coll_topo_connect(ucp_group_h group, ucp_group_member_index_t idx, ucp_ep_h *ep_p);

static UCS_F_ALWAYS_INLINE
enum ucp_coll_topo_type ucp_coll_topo_choose_type(enum ucp_group_collective_modifiers flags)
{
    if (flags & UCP_GROUP_COLLECTIVE_MODIFIER_SINGLE_SOURCE) {
        return UCP_COLL_TOPO_TREE_FANOUT;
    }

    if (flags & UCP_GROUP_COLLECTIVE_MODIFIER_SINGLE_DESTINATION) {
        return UCP_COLL_TOPO_TREE_FANIN;
    }

    if (flags & UCP_GROUP_COLLECTIVE_MODIFIER_AGGREGATE) {
        return UCP_COLL_TOPO_RECURSIVE;
    }

    return UCP_COLL_TOPO_TREE_FANIN_FANOUT;
}


/*
 * Per-topology prototypes
 */

ucs_status_t ucp_coll_topo_recursive_create(struct ucp_coll_topo_params *params, struct ucp_coll_topo **topo_p);

ucs_status_t ucp_coll_topo_tree_create(struct ucp_coll_topo_params *params, struct ucp_coll_topo **topo_p);

ucs_status_t ucp_coll_topo_tree_set_root(struct ucp_coll_topo *tree_topo, ucp_group_member_index_t root, struct ucp_coll_topo **topo_p);

#endif /* UCP_COLL_TOPO_H_ */
