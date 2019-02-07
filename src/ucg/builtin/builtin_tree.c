/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#include <math.h>
#include <ucs/debug/log.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/memtrack.h>
#include <uct/api/uct_def.h>
#include "../base/ucg_plan.h"

#define MAX_PEERS (100)

static inline ucs_status_t ucg_topo_connect_phase(ucg_topo_phase_t *phase,
        struct ucg_topo_params *params, ucp_ep_h **eps,
        ucg_group_member_index_t *peers, unsigned peer_cnt,
        enum ucg_topo_method_type method)
{
    ucs_status_t status = UCS_OK;
    unsigned peer_idx   = peer_cnt;
    phase->method       = method;
    phase->ep_cnt       = peer_cnt;
    phase->multi_eps    = *eps;
    ucp_ep_h *next_ep   = UCG_GET_EPS(phase);

    /* Intentionally done in reverse, list is in ascending order of distance */
    while (status == UCS_OK && peer_idx) {
        ucg_group_member_index_t next_peer = peers[--peer_idx];
        status = ucg_topo_connect(params->group, next_peer, next_ep++);
    }

    if (peer_cnt > 1) {
        *eps += peer_cnt;
    }

    return status;
}

static inline ucs_status_t ucg_topo_connect_tree(ucg_topo_t *tree,
        struct ucg_topo_params *params, size_t *alloc_size,
        ucg_group_member_index_t *up, unsigned up_cnt,
        ucg_group_member_index_t *down, unsigned down_cnt)
{
    ucs_status_t status          = UCS_OK;
    ucg_topo_phase_t *phase = &tree->phss[0];
    ucp_ep_h *eps                = (ucp_ep_h*)(phase + tree->phs_cnt);
    int has_children             = (up_cnt &&
            (params->type == UCG_TOPO_TREE_FANIN ||
             params->type == UCG_TOPO_TREE_FANIN_FANOUT));
    int has_parents              = (down_cnt &&
            (params->type == UCG_TOPO_TREE_FANOUT ||
             params->type == UCG_TOPO_TREE_FANIN_FANOUT));

    /* Phase #1: receive from children */
    if (has_children && status == UCS_OK) {
        /* Connect this phase to its peers */
        status = ucg_topo_connect_phase(phase++, params, &eps,
                down, down_cnt, UCG_TOPO_METHOD_REDUCE);
    }

    /* Phase #2: send to parents */
    if (has_parents && status == UCS_OK) {
        /* Connect this phase to its peers */
        status = ucg_topo_connect_phase(phase++, params, &eps,
                up, up_cnt, UCG_TOPO_METHOD_SEND);
    }

    /* Phase #3: receive from parents */
    if (has_parents && status == UCS_OK) {
        /* Connect this phase to its peers */
        status = ucg_topo_connect_phase(phase++, params, &eps,
                up, up_cnt, up_cnt > 1 ? UCG_TOPO_METHOD_REDUCE
                        : UCG_TOPO_METHOD_RECV);
    }

    /* Phase #4: send to children */
    if (has_children && status == UCS_OK) {
        /* Connect this phase to its peers */
        status = ucg_topo_connect_phase(phase++, params, &eps,
                down, down_cnt, UCG_TOPO_METHOD_SEND);
    }

    *alloc_size = (void*)eps - (void*)tree;
    if (status == UCS_OK) {
        return UCS_OK;
    }

    phase--;
    ucg_topo_destroy(tree);
    return status;
}

static ucs_status_t ucg_topo_tree_add_net(struct ucg_topo_params *params,
        ucg_group_member_index_t my_idx,
        ucg_group_member_index_t *up, unsigned *up_cnt,
        ucg_group_member_index_t *down, unsigned *down_cnt)
{
    /**
     * This is an attempt to auto-detect the mapping methods of members to cores
     * across the nodes in this group. The most common mapping method is
     * "by-node", where consecutive members are mapped on consecutive nodes.
     */
    int is_allocated_by_node           = 1;

    /**
     * We start by learning the layout - assuming it's symmetric on all nodes.
     * This member is a "host-master", possibly with socket-peers and host-peers.
     * We need to determine the number of processes per node (PPN) to calculate
     * which peers on the network are also host-masters.
     */
    unsigned peer_idx, socket_ppn = 1, ppn = 0;
    for (peer_idx = 0; peer_idx < *down_cnt; peer_idx++) {
        switch (params->group_params->distance[down[peer_idx]]) {
        case UCG_GROUP_MEMBER_DISTANCE_SOCKET:
            if (my_idx + peer_idx != down[peer_idx]) {
                is_allocated_by_node = 0;
            }
            socket_ppn++;
            break;

        case UCG_GROUP_MEMBER_DISTANCE_HOST:
            if (ppn == 0 && my_idx + peer_idx != down[peer_idx]) {
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
    ucg_group_member_index_t total = params->group_params->member_count;
    ucs_assert(total >= ppn);
    if (total == ppn) {
        return UCS_OK;
    }

    /* Calculate fabric peers up and down, assuming uniform distribution */
    unsigned range; /* The size of the currently handled sub-tree */
    if (is_allocated_by_node) { /* OMPI default */
        range = total / ppn;
        ppn   = 1; /* from now on - ignore non-host-master members. */
    } else {
        range = total;
    }

    /* Top-most nodes - multiroot exchange */
    ucg_group_member_index_t range_size, member_idx;
    ucg_group_member_index_t radix = params->tree_radix;
    range_size = range / radix + (range < radix);
    range_size += (range_size % ppn) ? ppn - (range_size % ppn) : 0; /* avoid non-host-masters */
    if (my_idx % range_size == 0) {
        for (member_idx = 0; member_idx < range; member_idx += range_size) {
            if (member_idx == my_idx) continue;
            up[(*up_cnt)++] = member_idx;
        }
    }

    /* Start under some top-phase root */
    range = range_size - ppn;
    unsigned offset = my_idx - (my_idx % range_size);
    unsigned overflow, prev_offset = offset;
    while (range >= ppn) {
        range_size = (range / radix);
        range_size += (range_size * radix < range);
        range_size += (range_size % ppn) ? ppn - (range_size % ppn) : 0; // avoid non-host-masters

        if (offset == my_idx) {
            /* fill in the single father */
            if (!*up_cnt) {
                up[(*up_cnt)++] = prev_offset;
            }

            /* fill with children */
            for (member_idx = offset + ppn;
                 member_idx <= offset + range;
                 member_idx += range_size * ppn) {
                down[(*down_cnt)++] = member_idx;
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
        offset = my_idx - ((my_idx - offset - ppn) % range_size);
        overflow = (offset - prev_offset) + range_size - range - ppn;
        if (overflow > 0) {
            range_size -= overflow;
        }
        range = range_size - ppn;

        ucs_assert(my_idx >= offset);
        ucs_assert(my_idx <= offset + range);
    }

    return UCS_OK;
}

static ucs_status_t ucg_topo_tree_build(struct ucg_topo_params *params,
        ucg_topo_t *tree, size_t *alloc_size)
{
    ucg_group_member_index_t my_idx;
    ucg_group_member_index_t member_idx;
    ucg_group_member_index_t up[MAX_PEERS];
    ucg_group_member_index_t down[MAX_PEERS];
    unsigned up_cnt = 0, down_cnt = 0;
    enum ucg_group_member_distance up_distance = UCG_GROUP_MEMBER_DISTANCE_LAST;
    enum ucg_group_member_distance down_distance = UCG_GROUP_MEMBER_DISTANCE_SELF;

    /**
     * "Master phase" would be the highest phase this member would be the master of.
     * By the end of this function, the master_phase represents the type of node
     * in the topology tree - one of the following:
     *
     * UCG_GROUP_MEMBER_DISTANCE_SELF:
     *         no children at all, father is socket-master
     *
     * UCG_GROUP_MEMBER_DISTANCE_SOCKET:
     *         socket-master, possible children on socket, father is host-master
     *
     * UCG_GROUP_MEMBER_DISTANCE_HOST:
     *         socket-and-host-master, possible socket-master children
     *         (one on each of the rest of the sockets) and other host-masters,
     *         father is a (single) host-master
     *
     * UCG_GROUP_MEMBER_DISTANCE_NET:
     *         fabric(-and-host-and-socket)-master, possible children of all
     *         types (but it's his own socket-and-host-master), fathers are
     *         a list of fabric-masters in a multi-root formation (topmost
     *         phase of each collective is all-to-all between fabric-masters)
     */
    enum ucg_group_member_distance master_phase = UCG_GROUP_MEMBER_DISTANCE_NET;

    /* Go over member distances, filling the per-phase member lists */
    int seen_myself = 0;
    for (member_idx = 0; member_idx < params->group_params->member_count; member_idx++) {
        enum ucg_group_member_distance next_distance =
                params->group_params->distance[member_idx];

        /* Possibly add the next member to my list according to its distance */
        if (ucs_unlikely(next_distance == UCG_GROUP_MEMBER_DISTANCE_SELF)) {
            seen_myself = 1;
            my_idx = member_idx;
        } else if (seen_myself) {
            /* Add children from phases I'm the master of */
            if ((next_distance <= master_phase) &&
                (next_distance >= down_distance) &&
                (next_distance != UCG_GROUP_MEMBER_DISTANCE_NET)) {
                down_distance = next_distance;
                down[down_cnt++] = member_idx;
                if (down_cnt == MAX_PEERS) {
                    ucs_error("Internal PPN limit (%i) exceeded", MAX_PEERS);
                    return UCS_ERR_UNSUPPORTED;
                }
            }
        } else {
            /* Check if this member is closer then the current parent */
            if ((up_distance > next_distance) || (member_idx)) {
                /* Replace parent, possibly "demoting" myself */
                up_distance  = next_distance;
                master_phase = next_distance - 1;
                up[up_cnt++] = member_idx;
                /**
                 * Note: in the "multi-root" case, members #1,2,3... are likely
                 * on the same host, and do not make for good tree roots. To
                 * address this we change the root selection - inside the call
                 * to @ref ucg_topo_fabric_calc .
                 */
            }
        }
    }

    /* Network peers calculation */
    if (master_phase >= UCG_GROUP_MEMBER_DISTANCE_HOST) {
        ucs_assert(up_cnt == 0); /* no parents (yet) */
        ucs_status_t ret = ucg_topo_tree_add_net(params,
                my_idx, up, &up_cnt, down, &down_cnt);
        if (ucs_unlikely(ret != UCS_OK)) {
            return ret;
        }
    }

    /* Some output, for informational purposes */
    ucs_info("Topology for member #%i (master_phase=%i):", my_idx, master_phase);
    for (member_idx = 0; member_idx < up_cnt; member_idx++) {
        ucs_info("%i's parent #%i / %u: %i ", my_idx, member_idx, up_cnt, up[member_idx]);
    }
    for (member_idx = 0; member_idx < down_cnt; member_idx++) {
        ucs_info("%i's child  #%i / %u: %i ", my_idx, member_idx, down_cnt, down[member_idx]);
    }

    /* fill in the tree phases while establishing the connections */
    tree->ep_cnt = 2 * (up_cnt + down_cnt);
    tree->phs_cnt = 2 * ((up_cnt > 0) + (down_cnt > 0));
    return ucg_topo_connect_tree(tree, params,
            alloc_size, up, up_cnt, down, down_cnt);
}

ucs_status_t ucg_builtin_tree_create(const ucg_group_params_t *group_params,
                                     const ucg_collective_params_t *coll_params,
                                     struct ucg_topo **topo_p)
{
    /* Allocate worst-case memory footprint, resized down later */
    size_t alloc_size = sizeof(ucg_topo_t) +
            4 * (sizeof(ucg_topo_phase_t) + MAX_PEERS * sizeof(uct_ep_h));
    ucg_topo_t *tree = (ucg_topo_t*)UCS_ALLOC_CHECK(alloc_size, "tree topology");
    tree->phs_cnt = 0; /* will be incremented with usage */

    /* tree discovery and construction, by phase */
    ucs_status_t ret = ucg_topo_tree_build(params, tree, &alloc_size);
    if (ret != UCS_OK) {
        ucs_free(tree);
        return ret;
    }

    /* Reduce the allocation size according to actual usage */
    *topo_p = (ucg_topo_t*)ucs_realloc(tree, alloc_size, "tree topology");
    ucs_assert(*topo_p != NULL); /* only reduces size - should never fail */
    return UCS_OK;
}


ucs_status_t ucg_topo_tree_set_root(struct ucg_topo *tree_topo, ucg_group_member_index_t root, struct ucg_topo **topo_p)
{
    return UCS_ERR_UNSUPPORTED; // TODO
}
