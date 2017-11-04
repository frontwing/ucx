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

#ifndef COLL_GROUP_H_
#define COLL_GROUP_H_

#include "ops.h"
#include "topo.h"

#include <ucs/datastruct/list.h>

typedef uint64_t ucp_group_id_t;

typedef struct ucp_coll_ctx {
	ucs_list_link_t groups;        /* List of currently active groups */
	unsigned        next_group_id; /* ID for the next group to be created */
} ucp_coll_ctx_t;

struct ucp_coll_group {
	ucp_group_create_params_t params;       /* Initial parameters, MPI Callbacks */
    ucp_group_id_t            group_id;     /* ID for this group */
    ucp_coll_id_t             next_coll_id; /* ID for the next collective to start */
	ucp_coll_topo_tree_t     *tree;         /* Topology Information */
    ucs_list_link_t           ops;          /* currently active ops (for resends) */

    int is_barrier_outstanding;             /* is a barrier (or ibarrier) running */

    /* cached ops to follow per collective */
    ucs_list_link_t op_lru_cache[UCP_GROUP_COLLECTIVE_TYPE_LAST];
};

/* Per-worker collective context */
ucs_status_t ucp_coll_init    (ucp_coll_ctx_t **ctx);
void         ucp_coll_finalize(ucp_coll_ctx_t  *ctx);

/* After a barrier-like function completes - this callback starts pending async collectives */
// TODO: optimization: call this after sending to children but before completion!
void ucp_coll_group_barrier_op_cb(ucp_coll_group_t *group, ucp_coll_op_t *op);

#endif /* COLL_GROUP_H_ */
