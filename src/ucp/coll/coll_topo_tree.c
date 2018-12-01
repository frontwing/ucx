#include "coll_topo.h"

#include <math.h>
#include <ucs/debug/log.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/memtrack.h>

#define MAX_PEERS (100)

static inline ucs_status_t ucp_coll_topo_connect_phase(ucp_coll_topo_phase_t *phase,
		struct ucp_coll_topo_params *params, ucp_ep_h **eps,
		ucp_group_rank_t *peers, unsigned peer_cnt,
		enum ucp_coll_topo_method_type method)
{
	ucs_status_t status = UCS_OK;
	unsigned peer_idx   = peer_cnt;
	phase->method       = method;
	phase->ep_cnt       = peer_cnt;
	phase->multi_eps    = *eps;
	ucp_ep_h *next_ep   = UCP_COLL_GET_EPS(phase);

	/* Intentionally done in reverse, list is in ascending order of distance */
	while (status == UCS_OK && peer_idx) {
		ucp_group_rank_t next_peer = peers[--peer_idx];
		status = ucp_coll_topo_connect(params->group, next_peer, next_ep++);
	}

	if (peer_cnt > 1) {
		*eps += peer_cnt;
	}

	return status;
}

static inline ucs_status_t ucp_coll_topo_connect_tree(ucp_coll_topo_t *tree,
		struct ucp_coll_topo_params *params, size_t *alloc_size,
		ucp_group_rank_t *up, unsigned up_cnt,
		ucp_group_rank_t *down, unsigned down_cnt)
{
	ucs_status_t status          = UCS_OK;
    ucp_coll_topo_phase_t *phase = &tree->phss[0];
    ucp_ep_h *eps                = (ucp_ep_h*)(phase + tree->phs_cnt);
    int has_children             = (up_cnt &&
    		(params->type == UCP_COLL_TOPO_TREE_FANIN ||
    		 params->type == UCP_COLL_TOPO_TREE_FANIN_FANOUT));
	int has_parents              = (down_cnt &&
    		(params->type == UCP_COLL_TOPO_TREE_FANOUT ||
    		 params->type == UCP_COLL_TOPO_TREE_FANIN_FANOUT));

	/* Phase #1: receive from children */
	if (has_children && status == UCS_OK) {
		/* Connect this phase to its peers */
		status = ucp_coll_topo_connect_phase(phase++, params, &eps,
				down, down_cnt, UCP_COLL_TOPO_METHOD_REDUCE);
	}

	/* Phase #2: send to parents */
	if (has_parents && status == UCS_OK) {
		/* Connect this phase to its peers */
		status = ucp_coll_topo_connect_phase(phase++, params, &eps,
				up, up_cnt, UCP_COLL_TOPO_METHOD_SEND);
	}

	/* Phase #3: receive from parents */
	if (has_parents && status == UCS_OK) {
		/* Connect this phase to its peers */
		status = ucp_coll_topo_connect_phase(phase++, params, &eps,
				up, up_cnt, up_cnt > 1 ? UCP_COLL_TOPO_METHOD_REDUCE
						: UCP_COLL_TOPO_METHOD_RECV);
	}

	/* Phase #4: receive from parents */
	if (has_children && status == UCS_OK) {
		/* Connect this phase to its peers */
		status = ucp_coll_topo_connect_phase(phase++, params, &eps,
				down, down_cnt, UCP_COLL_TOPO_METHOD_RECV);
	}

	*alloc_size = (void*)eps - (void*)tree;
	if (status == UCS_OK) {
		return UCS_OK;
	}

	phase--;
    ucp_coll_topo_destroy(tree);
    return status;
}

static ucs_status_t ucp_coll_topo_tree_add_net(struct ucp_coll_topo_params *params,
		ucp_group_rank_t *up, unsigned *up_cnt,
		ucp_group_rank_t *down, unsigned *down_cnt)
{
    /**
     * This is an attempt to auto-detect the mapping methods of ranks to cores
     * across the nodes in this group. The most common mapping method is
     * "by-node", where consecutive ranks are mapped on consecutive nodes.
     */
    int is_allocated_by_node           = 1;

	/**
	 * We start by learning the layout - assuming it's symmetric on all nodes.
	 * This rank is a "host-master", possibly with socket-peers and host-peers.
	 * We need to determine the number of processes per node (PPN) to calculate
	 * which peers on the network are also host-masters.
	 */
    unsigned peer_idx, socket_ppn = 1, ppn = 0;
    ucp_group_rank_t my_rank = params->group_params->my_rank_index;
    for (peer_idx = 0; peer_idx < *down_cnt; peer_idx++) {
    	switch (params->group_params->rank_distance[down[peer_idx]]) {
    	case UCP_GROUP_RANK_DISTANCE_SOCKET:
    		if (my_rank + peer_idx != down[peer_idx]) {
    			is_allocated_by_node = 0;
    		}
    		socket_ppn++;
    		break;

    	case UCP_GROUP_RANK_DISTANCE_HOST:
    		if (ppn == 0 && my_rank + peer_idx != down[peer_idx]) {
    			is_allocated_by_node = 0;
    		}
    		ppn += socket_ppn;
    		break;

    	default:
    		ucs_assert(0); /* should never happen */
    		return UCS_ERR_INVALID_PARAM;
    	}
    }
    ppn += socket_ppn;

    /* Check for intra-node-only topologies */
	ucp_group_rank_t total = params->group_params->total_ranks;
    ucs_assert(total >= ppn);
	if (total == ppn) {
        return UCS_OK;
    }

    /* Calculate fabric peers up and down, assuming uniform distribution */
	unsigned range; /* The size of the currently handled sub-tree */
    if (is_allocated_by_node) { /* OMPI default */
        range = total / ppn;
        ppn   = 1; /* from now on - ignore non-host-master ranks. */
    } else {
        range = total;
    }

    /* Top-most nodes - multiroot exchange */
    ucp_group_rank_t range_size, rank_idx;
    ucp_group_rank_t radix = params->tree_radix;
    range_size = range / radix + (range < radix);
    range_size += (range_size % ppn) ? ppn - (range_size % ppn) : 0; /* avoid non-host-masters */
    if (my_rank % range_size == 0) {
        for (rank_idx = 0; rank_idx < range; rank_idx += range_size) {
            if (rank_idx == my_rank) continue;
            up[(*up_cnt)++] = rank_idx;
        }
    }

    /* Start under some top-phase root */
    range = range_size - ppn;
    unsigned offset = my_rank - (my_rank % range_size);
    unsigned overflow, prev_offset = offset;
    while (range >= ppn) {
        range_size = (range / radix);
        range_size += (range_size * radix < range);
        range_size += (range_size % ppn) ? ppn - (range_size % ppn) : 0; // avoid non-host-masters

        if (offset == my_rank) {
            /* fill in the single father */
            if (!*up_cnt) {
            	up[(*up_cnt)++] = prev_offset;
            }

            /* fill with children */
            for (rank_idx = offset + ppn;
                 rank_idx <= offset + range;
                 rank_idx += range_size * ppn) {
                down[(*down_cnt)++] = rank_idx;
        		if (*down_cnt == MAX_PEERS) {
        			ucs_error("Internal PPN limit (%i) exceeded", MAX_PEERS);
        			return UCS_ERR_UNSUPPORTED;
        		}
            }
        } else if ((range_size == ppn) && (!*up_cnt)) {
            /* fill in the single father */
        	up[(*up_cnt)++] = offset;
        }

        /* update the iterators */
        prev_offset = offset;
        offset = my_rank - ((my_rank - offset - ppn) % range_size);
        overflow = (offset - prev_offset) + range_size - range - ppn;
        if (overflow > 0) {
            range_size -= overflow;
        }
        range = range_size - ppn;

        ucs_assert(my_rank >= offset);
        ucs_assert(my_rank <= offset + range);
    }

    return UCS_OK;
}

static ucs_status_t ucp_coll_topo_tree_build(struct ucp_coll_topo_params *params,
		ucp_coll_topo_t *tree, size_t *alloc_size)
{
    ucp_group_rank_t rank_idx;
    ucp_group_rank_t up[MAX_PEERS];
    ucp_group_rank_t down[MAX_PEERS];
    unsigned up_cnt = 0, down_cnt = 0;
    enum ucp_group_rank_distance up_distance = UCP_GROUP_RANK_DISTANCE_LAST;
    enum ucp_group_rank_distance down_distance = UCP_GROUP_RANK_DISTANCE_SELF;

	/**
	 * "Master phase" would be the highest phase this rank would be the master of.
     * By the end of this function, the master_phase represents the type of node
     * in the topology tree - one of the following:
     *
     * UCP_GROUP_RANK_DISTANCE_SELF:
     * 		no children at all, father is socket-master
     *
     * UCP_GROUP_RANK_DISTANCE_SOCKET:
     * 		socket-master, possible children on socket, father is host-master
     *
     * UCP_GROUP_RANK_DISTANCE_HOST:
     * 		socket-and-host-master, possible socket-master children
     * 		(one on each of the rest of the sockets) and other host-masters,
     * 		father is a (single) host-master
     *
     * UCP_GROUP_RANK_DISTANCE_NET:
     * 		fabric(-and-host-and-socket)-master, possible children of all
     * 		types (but it's his own socket-and-host-master), fathers are
     * 		a list of fabric-masters in a multi-root formation (topmost
     * 		phase of each collective is all-to-all between fabric-masters)
     */
    enum ucp_group_rank_distance master_phase = UCP_GROUP_RANK_DISTANCE_NET;

    /* Go over rank distances, filling the per-phase rank lists */
    int seen_myself = 0;
    for (rank_idx = 0; rank_idx < params->group_params->total_ranks; rank_idx++) {
    	enum ucp_group_rank_distance next_distance =
    			params->group_params->rank_distance[rank_idx];

    	/* Possibly add the next rank to my list according to its distance */
        if (ucs_unlikely(next_distance == UCP_GROUP_RANK_DISTANCE_SELF)) {
            seen_myself = 1;
        } else if (seen_myself) {
        	/* Add children from phases I'm the master of */
        	if ((next_distance <= master_phase) &&
        		(next_distance >= down_distance) &&
        		(next_distance != UCP_GROUP_RANK_DISTANCE_NET)) {
        		down_distance = next_distance;
        		down[down_cnt++] = rank_idx;
        		if (down_cnt == MAX_PEERS) {
        			ucs_error("Internal PPN limit (%i) exceeded", MAX_PEERS);
        			return UCS_ERR_UNSUPPORTED;
        		}
        	}
        } else {
        	/* Check if this rank is closer then the current parent */
        	if ((up_distance > next_distance) || (rank_idx)) {
        		/* Replace parent, possibly "demoting" myself */
        		up_distance  = next_distance;
        		master_phase = next_distance - 1;
        		up[up_cnt++] = rank_idx;
        		/**
        		 * Note: in the "multi-root" case, ranks #1,2,3... are likely
        		 * on the same host, and do not make for good tree roots. To
        		 * address this we change the root selection - inside the call
        		 * to @ref ucp_coll_topo_fabric_calc .
        		 */
        	}
        }
    }

    /* Network peers calculation */
    if (master_phase >= UCP_GROUP_RANK_DISTANCE_HOST) {
    	ucs_assert(up_cnt == 0); /* no parents (yet) */
        ucs_status_t ret = ucp_coll_topo_tree_add_net(params,
        		up, &up_cnt, down, &down_cnt);
        if (ucs_unlikely(ret != UCS_OK)) {
        	return ret;
        }
    }

    /* Some output, for informational purposes */
    ucs_info("Topology for rank #%i (master_phase=%i):", params->group_params->my_rank_index, master_phase);
    for (rank_idx = 0; rank_idx < up_cnt; rank_idx++) {
    	ucs_info("%i's parent #%i / %u: %i ", params->group_params->my_rank_index, rank_idx, up_cnt, up[rank_idx]);
    }
    for (rank_idx = 0; rank_idx < down_cnt; rank_idx++) {
    	ucs_info("%i's child  #%i / %u: %i ", params->group_params->my_rank_index, rank_idx, down_cnt, down[rank_idx]);
    }

    /* fill in the tree phases while establishing the connections */
    tree->ep_cnt = 2 * (up_cnt + down_cnt);
    tree->phs_cnt = 2 * ((up_cnt > 0) + (down_cnt > 0));
    return ucp_coll_topo_connect_tree(tree, params,
    		alloc_size, up, up_cnt, down, down_cnt);
}

ucs_status_t ucp_coll_topo_tree_create(struct ucp_coll_topo_params *params, struct ucp_coll_topo **topo_p)
{
	/* Allocate worst-case memory footprint, resized down later */
	size_t alloc_size = sizeof(ucp_coll_topo_t) +
			4 * (sizeof(ucp_coll_topo_phase_t) + MAX_PEERS * sizeof(uct_ep_h));
	ucp_coll_topo_t *tree = (ucp_coll_topo_t*)UCS_ALLOC_CHECK(alloc_size, "tree topology");
	tree->phs_cnt = 0; /* will be incremented with usage */

	/* tree discovery and construction, by phase */
	ucs_status_t ret = ucp_coll_topo_tree_build(params, tree, &alloc_size);
	if (ret != UCS_OK) {
		ucs_free(tree);
		return ret;
	}

	/* Reduce the allocation size according to actual usage */
	*topo_p = (ucp_coll_topo_t*)ucs_realloc(tree, alloc_size, "tree topology");
	ucs_assert(*topo_p != NULL); /* only reduces size - should never fail */
	return UCS_OK;
}


ucs_status_t ucp_coll_topo_tree_set_root(struct ucp_coll_topo *tree_topo, ucp_group_rank_t root, struct ucp_coll_topo **topo_p)
{
	return UCS_ERR_UNSUPPORTED; // TODO
}
