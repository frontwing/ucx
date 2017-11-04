/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2012.  ALL RIGHTS RESERVED.
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

#ifndef COLL_TOPO_H_
#define COLL_TOPO_H_

#include "req.h"

enum ucp_coll_topo_map_flag {
    UCP_COLL_TOPO_MAP_FLAG_MASTER = 0,
    UCP_COLL_TOPO_MAP_FLAG_MULTI_ROOT,
};

#define UCP_COLL_TOPO_MAP_IS_MASTER(flags)    (flags & UCS_BIT(UCP_COLL_TOPO_MAP_FLAG_MASTER))
#define UCP_COLL_TOPO_MAP_IS_MULTIROOT(flags) (flags & UCS_BIT(UCP_COLL_TOPO_MAP_FLAG_MULTI_ROOT))

struct ucp_coll_topo_map {
	/* TODO: extend to support iface, e.g. UD */
    uct_ep_h               *eps;
    unsigned                ep_cnt;
    enum ucp_coll_topo_map_flag flags;
};

enum ucp_coll_topo_phases {
    UCP_COLL_TOPO_PHASE_SOCKET_FANIN = 0,
    UCP_COLL_TOPO_PHASE_HOST_FANIN,
    UCP_COLL_TOPO_PHASE_FABRIC_DOWN_FANIN,
    UCP_COLL_TOPO_PHASE_FABRIC_UP_FANIN,
    UCP_COLL_TOPO_PHASE_FABRIC_FULL_FANOUT,
    UCP_COLL_TOPO_PHASE_FABRIC_AUX_FANOUT, // only if bcast is disabled
    UCP_COLL_TOPO_PHASE_HOST_FANOUT,
    UCP_COLL_TOPO_PHASE_SOCKET_FANOUT,

	UCP_COLL_TOPO_PHASE_LAST,

    UCP_COLL_TOPO_EXTRA_MAP_TARGET,
    UCP_COLL_TOPO_EXTRA_MAP_NEIGHBOR,

	UCP_COLL_TOPO_EXTRA_MAP_LAST
};

struct ucp_coll_topo_tree {
	/* Store the list of processes for custom-root reduce operations */
	ucp_coll_proc_h          *procs;      /* array of process */
    unsigned                  proc_count; /* size of process array */
    ucp_group_rank_t          my_rank;    /* local process index in array */
    ucp_group_resolve_f       resolve;    /* rank-to-endpoint resolving callback */

    /* instructions to follow per collective (contig + fragmented) */
    ucs_list_link_t           instructions[2 * UCP_GROUP_COLLECTIVE_TYPE_LAST];
    // TODO: need to replace this with ascend/descend/neighbor

    /* Maps for all stages of the collective */
    unsigned                  map_count;    /* valid map count, excluding the "extra maps" */
    enum ucp_coll_topo_phases fanout_map;   /* index of first map to start fanout */
    ucp_coll_topo_map_t       maps[UCP_COLL_TOPO_PHASE_LAST + UCP_COLL_TOPO_EXTRA_MAP_LAST];
};

ucs_status_t ucp_coll_topo_tree_create (const ucp_group_create_params_t *params,
		                                ucp_coll_topo_tree_t **tree_p);
void         ucp_coll_topo_tree_destroy(ucp_coll_topo_tree_t *tree);

ucs_status_t ucp_coll_topo_generate_target_map(ucp_coll_topo_tree_t *tree,
		ucp_group_rank_t root, ucp_coll_topo_map_t **map);

#endif /* COLL_TOPO_H_ */
