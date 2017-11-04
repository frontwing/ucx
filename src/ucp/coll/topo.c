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

#include "ops.h"
#include "topo.h"

#include <ucs/debug/assert.h>
#include <ucs/debug/memtrack.h>
#include <ucs/datastruct/list.h>

#include <ucp/core/ucp_ep.h> /* to obtain uct_ep */

#define MAX_PROCS_PER_MACHINE (32)

static ucs_status_t ucp_coll_topo_get_ep(ucp_coll_topo_tree_t *tree,
		ucp_group_rank_t rank, uct_ep_h *result_ep)
{
	ucp_ep_h ep;
	ucs_status_t res = tree->resolve(rank, &ep);
	if (res != UCS_OK) {
		return res;
	}

	*result_ep = ep->uct_eps[ep->am_lane]; // TODO: generalize...
	return UCS_OK;
}

ucs_status_t ucp_coll_topo_generate_target_map(ucp_coll_topo_tree_t *tree,
		ucp_group_rank_t root, ucp_coll_topo_map_t **map)
{
	ucp_coll_topo_map_t *res = UCS_ALLOC_CHECK(sizeof(ucp_coll_topo_map_t) +
			sizeof(uct_ep_h), "target_map");
	res->ep_cnt              = 1;
	res->eps                 = (uct_ep_h*)(res + 1);
	res->flags               = (tree->my_rank == root) ?
			UCS_BIT(UCP_COLL_TOPO_MAP_FLAG_MASTER) : 0;

	ucs_status_t error = ucp_coll_topo_get_ep(tree, root, res->eps);
	if (error != UCS_OK) {
		ucs_free(res);
		return error;
	}

	*map = res;
	return UCS_OK;
}

static inline ucs_status_t create_extra_map_neighbor(ucp_coll_topo_tree_t *tree,
		const ucp_group_create_params_t *params)
{
	ucs_status_t error;
    unsigned proc_idx, proc_sqrt = (unsigned)sqrt(params->proc_count);
    ucp_coll_topo_map_t *neighbor_map = &tree->maps[UCP_COLL_TOPO_PHASE_LAST +
													UCP_COLL_TOPO_EXTRA_MAP_NEIGHBOR];
    neighbor_map->eps = UCS_ALLOC_CHECK(4 * sizeof(uct_ep_t),
                    "neighbor connections");

    neighbor_map->flags = UCS_BIT(UCP_COLL_TOPO_MAP_FLAG_MULTI_ROOT);
    neighbor_map->ep_cnt = 4;

    proc_idx = (params->my_rank + params->proc_count - proc_sqrt) % params->proc_count;
    error = ucp_coll_topo_get_ep(tree, proc_idx, &neighbor_map->eps[0]);  // up
    if (error != UCS_OK) {
    	return error;
    }

    proc_idx = (params->my_rank + proc_sqrt) % params->proc_count;
    error = ucp_coll_topo_get_ep(tree, proc_idx, &neighbor_map->eps[1]); // down
    if (error != UCS_OK) {
    	return error;
    }

    proc_idx = (params->my_rank % proc_sqrt) ?
            (params->my_rank + params->proc_count - 1) % params->proc_count :
            params->my_rank + proc_sqrt;
    error = ucp_coll_topo_get_ep(tree, proc_idx, &neighbor_map->eps[2]);  // left
    if (error != UCS_OK) {
    	return error;
    }

    proc_idx = ((params->my_rank + 1) % proc_sqrt) ?
            (params->my_rank + 1) % params->proc_count :
            (params->my_rank + params->proc_count - proc_sqrt) % params->proc_count;
    return ucp_coll_topo_get_ep(tree, proc_idx, &neighbor_map->eps[3]);  // right
}

static inline ucs_status_t create_extra_map_target(ucp_coll_topo_tree_t *tree,
        const ucp_group_create_params_t *params,
        enum ucp_coll_distance master_level,
        ucp_group_rank_t *fabric_peers_down,
        unsigned fabric_peer_down_cnt)
{
    unsigned proc_idx;
    ucp_coll_topo_map_t *target_map = &tree->maps[UCP_COLL_TOPO_PHASE_LAST +
												  UCP_COLL_TOPO_EXTRA_MAP_TARGET];
    target_map->eps = UCS_ALLOC_CHECK(fabric_peer_down_cnt * sizeof(uct_ep_t),
                    "target connections");

    target_map->flags = 0;
    target_map->ep_cnt = fabric_peer_down_cnt;
    for (proc_idx = 0; proc_idx < fabric_peer_down_cnt; proc_idx++) {
        ucp_group_rank_t root_rank = fabric_peers_down[proc_idx];
        ucs_status_t error = ucp_coll_topo_get_ep(tree, root_rank, &target_map->eps[0]);
        if (error != UCS_OK) {
        	return error;
        }

        if ((master_level == UCP_COLL_DISTANCE_HOST) && (params->my_rank == root_rank)) {
            target_map->flags = UCP_COLL_TOPO_MAP_FLAG_MASTER;
        }
    }

    return UCS_OK;
}

static int ucp_coll_topo_fabric_calc(const ucp_group_create_params_t *params,
        int is_allocated_by_node, unsigned socket_peer_cnt, unsigned host_peer_cnt,
        ucp_group_rank_t *fabric_peers_up, ucp_group_rank_t *fabric_peers_down,
        unsigned *fabric_peer_up_cnt, unsigned *fabric_peer_down_cnt)
{
    // calculate fabric peers up and down, assuming uniform distribution
    unsigned range_size, proc_idx;
    unsigned ppn = 1 + socket_peer_cnt + host_peer_cnt * (1 + socket_peer_cnt);
    unsigned prev_offset, offset, range;
    int is_multi_root = 0;
    int overflow;

    // all procs on one node (no IB)
    if (params->proc_count == ppn) {
        return 0;
    }

    if (is_allocated_by_node) { // OMPI default
        ucs_assert((params->proc_count % ppn) == 0);
        // TODO: deal with non-uniform allocations
        range = params->proc_count / ppn;
        ppn = 1; // from now on - ignore non-host-master ranks.
    } else {
        range = params->proc_count;
    }

    // top-most nodes - multiroot exchange
    range_size = range / params->tree.radix + (range < params->tree.radix);
    range_size += (range_size % ppn) ? ppn - (range_size % ppn) : 0; // avoid non-host-masters
    if (params->my_rank % range_size == 0) {
        is_multi_root = 1;
        for (proc_idx = 0; proc_idx < range; proc_idx += range_size) {
            if (proc_idx == params->my_rank) continue;
            fabric_peers_up[(*fabric_peer_up_cnt)++] = proc_idx;
        }
    }

    // start under some top-level root
    range = range_size - ppn;
    prev_offset = offset = params->my_rank - (params->my_rank % range_size);

    while (range >= ppn) {
        range_size = (range / params->tree.radix);
        range_size += (range_size * params->tree.radix < range);
        range_size += (range_size % ppn) ? ppn - (range_size % ppn) : 0; // avoid non-host-masters

        if (offset == params->my_rank) {
            // fill in the single father
            if (!*fabric_peer_up_cnt) {
                fabric_peers_up[(*fabric_peer_up_cnt)++] = prev_offset;
            }

            // fill with children
            for (proc_idx = offset + ppn;
                 proc_idx <= offset + range;
                 proc_idx += range_size * ppn) {
                fabric_peers_down[(*fabric_peer_down_cnt)++] = proc_idx;
            }
        } else if ((range_size == ppn) && (!*fabric_peer_up_cnt)) {
            // fill in the single father
            fabric_peers_up[(*fabric_peer_up_cnt)++] = offset;
        }

        // update the iterators
        prev_offset = offset;
        offset = params->my_rank - ((params->my_rank - offset - ppn) % range_size);
        overflow = (offset - prev_offset) + range_size - range - ppn;
        if (overflow > 0) {
            range_size -= overflow;
        }
        range = range_size - ppn;

        ucs_assert(params->my_rank >= offset);
        ucs_assert(params->my_rank <= offset + range);
    }
    return is_multi_root;
}

static ucs_status_t ucp_coll_topo_connect_peers(ucp_coll_topo_tree_t *tree,
		unsigned num_peers, ucp_group_rank_t *peers, ucp_coll_proc_h *all_procs,
		ucp_coll_topo_map_t *map)
{
    unsigned proc_idx;
    ucs_status_t error = UCS_OK;

    if (map->ep_cnt) {
        map->eps = ucs_realloc(map->eps,
                (map->ep_cnt + num_peers) * sizeof(uct_ep_t));
        if (map->eps == NULL) {
            return UCS_ERR_NO_MEMORY;
        }
    } else {
        map->eps = UCS_ALLOC_CHECK(num_peers * sizeof(uct_ep_t),
                    "connections");
    }

    for (proc_idx = 0;
         ((proc_idx < num_peers) && (error == UCS_OK));
         proc_idx++) {
        ucp_group_rank_t peer_rank = peers[proc_idx];
        error = ucp_coll_topo_get_ep(tree, peer_rank,
                &map->eps[map->ep_cnt + proc_idx]);
    }

    map->ep_cnt += num_peers;
    return error;
}

void ucp_coll_topo_dispose(ucp_coll_topo_tree_t *tree)
{
    enum ucp_coll_topo_phases phase_iter;
    for (phase_iter = 0;
         phase_iter < (tree->map_count + UCP_COLL_TOPO_EXTRA_MAP_LAST);
         phase_iter++) {
        ucs_free(tree->maps[phase_iter].eps);
    }
    ucs_free(tree);
}

static ucs_status_t ucp_coll_topo_connect_tree(ucp_coll_topo_tree_t *new_tree,
		const ucp_group_create_params_t *params,
		enum ucp_coll_distance master_level,
        unsigned *socket_peers, unsigned socket_peer_cnt,
        unsigned *host_peers, unsigned host_peer_cnt,
        unsigned *fabric_peers_up, unsigned fabric_peer_up_cnt,
        unsigned *fabric_peers_down, unsigned fabric_peer_down_cnt)
{
    ucs_status_t error;
    enum ucp_coll_topo_phases phase_iter;
    ucp_coll_topo_map_t *current_map;
    int is_bcast_disabled = 1;

    unsigned *all_peers[] = {
            socket_peers, host_peers,
            fabric_peers_down,
            fabric_peers_up,   // applies to non-roots only
            fabric_peers_up,   // applies to roots only
            fabric_peers_down, // applies only if bcast is disabled
            host_peers, socket_peers
    };

    unsigned all_peer_counts[] = {
            socket_peer_cnt,
            (master_level > UCP_COLL_DISTANCE_ME) ? host_peer_cnt : 0,
            fabric_peer_down_cnt,
            ((master_level == UCP_COLL_DISTANCE_HOST) || is_bcast_disabled) ? fabric_peer_up_cnt : 0,
            ((master_level == UCP_COLL_DISTANCE_FABRIC) && !is_bcast_disabled) ? fabric_peer_up_cnt : 0,
            is_bcast_disabled ? fabric_peer_down_cnt : 0,
            (master_level > UCP_COLL_DISTANCE_ME) ? host_peer_cnt : 0,
            socket_peer_cnt
    };

    int i;
    for (i = 0; i < sizeof(all_peer_counts)/sizeof(unsigned); i++) {
    	printf("#%i ucp_coll_topo_connect_tree[%i]=%i\n", getpid(), i, all_peer_counts[i]);
    }

    // adjust master-level to fit array slots above
    if (master_level >= UCP_COLL_DISTANCE_HOST) {
        master_level += 1;
    }

    // init the maps to be truncated
    for (phase_iter = 0;
         phase_iter < UCP_COLL_TOPO_PHASE_LAST;
         phase_iter++) {
        new_tree->maps[phase_iter].ep_cnt = 0;
    }

    // start by allocating for connections and sending master addresses
    new_tree->fanout_map = (unsigned)-1;
    current_map = &new_tree->maps[0];
    for (phase_iter = 0;
         phase_iter < UCP_COLL_TOPO_PHASE_LAST;
         phase_iter++) {

        if (phase_iter == UCP_COLL_TOPO_PHASE_FABRIC_FULL_FANOUT) {
            current_map++;
            if (new_tree->fanout_map == (unsigned)-1) {
                new_tree->fanout_map = current_map - &new_tree->maps[0];
               // if (params->my_rank == 0)
               printf("%i: TOPO - PEAK (fanout_map=%i)------------------------\n", params->my_rank, new_tree->fanout_map);
            }
        }

        //printf("level=%i peers=%i prev_is_master=%i current_map_count=%i\n", phase_iter, all_peer_counts[phase_iter], prev_is_master, current_map->ep_cnt);
        if (all_peer_counts[phase_iter]) {
            // determine if it's a master on this level
        	enum ucp_coll_topo_map_flag flags = 0;
            int is_master = (phase_iter > UCP_COLL_TOPO_PHASE_FABRIC_FULL_FANOUT) ?
                    (master_level >= (UCP_COLL_TOPO_PHASE_LAST - phase_iter)) :
                    ((unsigned)phase_iter < (unsigned)master_level);

            // check for multi-root case
            if ((master_level == UCP_COLL_DISTANCE_LAST) &&
                is_bcast_disabled &&
                (phase_iter == UCP_COLL_TOPO_PHASE_FABRIC_UP_FANIN)) {
                flags = UCS_BIT(UCP_COLL_TOPO_MAP_FLAG_MULTI_ROOT);
            } else if (is_master) {
                flags = UCS_BIT(UCP_COLL_TOPO_MAP_FLAG_MASTER);
            }

            if ((current_map->ep_cnt) && (flags != current_map->flags)) {
                current_map++;
            }

            if (UCP_COLL_TOPO_MAP_IS_MULTIROOT(flags)) {
                new_tree->fanout_map = current_map - &new_tree->maps[0];
                //if (params->my_rank == 0)
                	printf("%i: TOPO - MULTIROOT PEAK (fanout_map=%i)------------------------\n", params->my_rank, new_tree->fanout_map);
            }

            current_map->flags = flags;
            error = ucp_coll_topo_connect_peers(new_tree, all_peer_counts[phase_iter],
            		all_peers[phase_iter], params->procs, current_map);
            if (error != UCS_OK) {
                goto connect_tree_cleanup;
            }

            //if (params->my_rank == 0)
            	printf("%i: TOPO - [phase=%i]: %i peers, is_master=%i, map=%p\n", params->my_rank, phase_iter, all_peer_counts[phase_iter], is_master, current_map);
        }
    }

    // close the standard map count and move up the extra maps
    if (current_map->ep_cnt) {
        current_map++;
    }
    new_tree->map_count = current_map - &new_tree->maps[0];
    if (new_tree->map_count < UCP_COLL_TOPO_PHASE_LAST) {
        for (phase_iter = 0;
             phase_iter < UCP_COLL_TOPO_EXTRA_MAP_LAST;
             phase_iter++, current_map++) {
            *current_map =
            		new_tree->maps[UCP_COLL_TOPO_PHASE_LAST + phase_iter];
        }
    }

    printf("%i ucp_coll_topo_connect_tree: fanount_map=%i total_maps=%i\n", params->my_rank, new_tree->fanout_map, new_tree->map_count);
    return UCS_OK;

connect_tree_cleanup:
    ucp_coll_topo_dispose(new_tree);
    return error;
}

ucs_status_t ucp_coll_topo_discover(const ucp_group_create_params_t *params,
                                    ucp_coll_topo_tree_t **tree)
{
    unsigned proc_idx;
    int is_me_found = 0;
    int is_allocated_by_node = 1;
    ucs_status_t error = UCS_OK;
    ucp_group_rank_t *socket_peers, *host_peers;
    ucp_group_rank_t *fabric_peers_up, *fabric_peers_down;
    unsigned socket_peer_cnt = 0, host_peer_cnt = 0;
    unsigned fabric_peer_up_cnt = 0, fabric_peer_down_cnt = 0;
    enum ucp_coll_distance master_level = UCP_COLL_DISTANCE_ME;

    ucp_coll_topo_tree_t *new_tree = UCS_ALLOC_CHECK(sizeof(*new_tree),
            "topology tree");

    socket_peers = UCS_ALLOC_CHECK(MAX_PROCS_PER_MACHINE * sizeof(ucp_group_rank_t), "topo_socket_peers");
    host_peers = UCS_ALLOC_CHECK(MAX_PROCS_PER_MACHINE * sizeof(ucp_group_rank_t), "topo_host_peers");
    fabric_peers_up = UCS_ALLOC_CHECK(params->tree.radix * sizeof(ucp_group_rank_t), "topo_fabric_up_peers");
    fabric_peers_down = UCS_ALLOC_CHECK(params->tree.radix * sizeof(ucp_group_rank_t), "topo_fabric_down_peers");
    if ((host_peers == NULL) ||
        (socket_peers == NULL) ||
        (fabric_peers_up == NULL) ||
        (fabric_peers_down == NULL)) {
        ucs_free(new_tree);
        return UCS_ERR_NO_MEMORY;
    }

    new_tree->procs = params->procs;
    new_tree->proc_count = params->proc_count;

	if (new_tree->proc_count < 2) {
		ucs_error("At least two processes are required for collective operations");
		return UCS_ERR_INVALID_PARAM;
	}

    // scan for local peers
    for (proc_idx = 0; proc_idx < params->proc_count; proc_idx++) {
        switch (params->distances[proc_idx]) {
        case UCP_COLL_DISTANCE_ME:
        	if (is_me_found) {
        		ucs_error("More than one process of distance UCP_COLL_DISTANCE_ME");
        		return UCS_ERR_INVALID_PARAM;
        	}
        	is_me_found = 1;
            if (socket_peer_cnt == 0) {
                if (host_peer_cnt == 0) {
                    master_level = UCP_COLL_DISTANCE_HOST;
                } else {
                    master_level = UCP_COLL_DISTANCE_SOCKET;
                }
            }
            break;

        case UCP_COLL_DISTANCE_SOCKET:
            if ((socket_peer_cnt == 0) ||
                    (master_level != UCP_COLL_DISTANCE_ME)) {
                socket_peers[socket_peer_cnt++] = proc_idx;
            } else {
                socket_peer_cnt++;
            }

            if ((proc_idx + 1 == params->my_rank) ||
                (proc_idx == params->my_rank + 1)) {
                is_allocated_by_node = 0;
            }
            break;

        case UCP_COLL_DISTANCE_HOST:
            if ((host_peer_cnt == 0) ||
                    (master_level == UCP_COLL_DISTANCE_HOST)) {
                host_peers[host_peer_cnt++] = proc_idx;
            } else {
                host_peer_cnt++;
            }

            if ((proc_idx + 1 == params->my_rank) ||
                (proc_idx == params->my_rank + 1)) {
                is_allocated_by_node = 0;
            }
            break;

        default:
            break;
        }
    }

	if (!is_me_found) {
		ucs_error("No process of distance UCP_COLL_DISTANCE_ME");
		return UCS_ERR_INVALID_PARAM;
	}

    // cut down on host-level peers assuming they are socket-level peers
    ucs_assert((host_peer_cnt % (socket_peer_cnt + 1)) == 0);
    // TODO: deal with non-uniform allocations
    host_peer_cnt /= socket_peer_cnt + 1;
    // TODO: support more then two sockets

    // create special neighborhood map
    error = create_extra_map_neighbor(new_tree, params);
    if (error != UCS_OK) {
        return error;
    }

    // fabric peers calculation
    if (master_level == UCP_COLL_DISTANCE_HOST) {
        master_level += ucp_coll_topo_fabric_calc(params,
                is_allocated_by_node,
                socket_peer_cnt, host_peer_cnt,
                fabric_peers_up, fabric_peers_down,
                &fabric_peer_up_cnt, &fabric_peer_down_cnt);
    } else {
        // now that calculations are done - restore real local peer counts
        if ((master_level < UCP_COLL_DISTANCE_HOST) && host_peer_cnt) {
            host_peer_cnt = 1;
        }
        if ((master_level < UCP_COLL_DISTANCE_SOCKET) && socket_peer_cnt) {
            socket_peer_cnt = 1;
        }
    }


    if (0) {//(params->my_rank % 4 == 0) {
        printf ("%i: TOPO: (master_level=%i)", params->my_rank, master_level);
        printf ("\n%i: socket (%u peers): ", params->my_rank, socket_peer_cnt);
        for (proc_idx = 0; proc_idx < socket_peer_cnt; proc_idx++) {
            printf("%u,", socket_peers[proc_idx]);
        }
        printf ("\n%i: host (%u peers): ", params->my_rank, host_peer_cnt);
        for (proc_idx = 0; proc_idx < host_peer_cnt; proc_idx++) {
            printf("%u,", host_peers[proc_idx]);
        }
        printf ("\n%i: children (%u peers): ", params->my_rank, fabric_peer_down_cnt);
        for (proc_idx = 0; proc_idx < fabric_peer_down_cnt; proc_idx++) {
            printf("%u,", fabric_peers_down[proc_idx]);
        }
        printf ("\n%i: fathers (%u peers): ", params->my_rank, fabric_peer_up_cnt);
        for (proc_idx = 0; proc_idx < fabric_peer_up_cnt; proc_idx++) {
            printf("%u,", fabric_peers_up[proc_idx]);
        }
        printf("\n");
    }

    /*
     * By this stage, the master_level represents the type of node
     * in the topology tree, one of the following:
     * 0 - no children at all, father is socket-master
     * 1 - socket-master, possible children on socket, father is host-master
     * 2 - socket-and-host-master, possible socket-master children
     *     (one on each of the rest of the sockets) and other host-masters,
     *     father is a (single) host-master
     * 3 - fabric(-and-host-and-socket)-master, possible children of all
     *     types (but it's his own socket-and-host-master), fathers are
     *     a list of fabric-masters in a multi-root formation (topmost
     *     level of each collective is all-to-all between fabric-masters)
     */

    // create special multi-root map
    error = create_extra_map_target(new_tree, params, master_level,
            fabric_peers_down, fabric_peer_down_cnt);
    if (error != UCS_OK) {
        return error;
    }

    // fill in the tree maps while establishing the connections
    error = ucp_coll_topo_connect_tree(new_tree, params,
            master_level,
            socket_peers, socket_peer_cnt,
            host_peers, host_peer_cnt,
            fabric_peers_up, fabric_peer_up_cnt,
            fabric_peers_down, fabric_peer_down_cnt);

    if (error != UCS_OK) {
        ucs_free(new_tree);
        return error;
    }

    *tree = new_tree;
    return UCS_OK;
}

ucs_status_t ucp_coll_common_instructions_add(ucs_list_link_t *instructions_head,
        ucp_coll_topo_map_t* map, ucp_coll_step_maker_f direct,
        ucp_step_complete_cb_f direct_cb, ucp_coll_req_flags_t flags)
{
    ucp_coll_instruction_t *instruction =
            UCS_ALLOC_CHECK(sizeof(ucp_coll_instruction_t), "instruction");

    instruction->map       = map;
    instruction->make_step = direct;
    instruction->comp_cb   = direct_cb;
    instruction->flags     = flags;

    ucs_list_insert_before(instructions_head, &instruction->list);
    return UCS_OK;
}

void ucp_coll_destroy_traversal(ucs_list_link_t *instructions_head)
{
    while (!ucs_list_is_empty(instructions_head)) {
        ucs_free(ucs_list_extract_head(instructions_head,
                ucp_coll_instruction_t, list));
    }
}

/* Currently - the only supported topology is a tree... this will change. */
ucs_status_t ucp_coll_topo_tree_create(const ucp_group_create_params_t *params,
									   ucp_coll_topo_tree_t **tree)
{
    /* go ahead and build a new tree */
    ucs_status_t error = ucp_coll_topo_discover(params, tree);
    if (error != UCS_OK) {
    	return error;
    }

    return ucp_coll_init_tree_ops(*tree);
}

void ucp_coll_topo_tree_destroy(ucp_coll_topo_tree_t *tree)
{
	ucp_coll_cleanup_tree_ops(tree);
    ucp_coll_topo_dispose(tree);
}
