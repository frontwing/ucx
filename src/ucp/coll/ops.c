/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2013.  ALL RIGHTS RESERVED.
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

#include "group.h" // not ops.h because of ucp_coll_group_complete_sync

ucp_group_collective_extended_modifiers_t coll_construct_flags[UCP_GROUP_COLLECTIVE_TYPE_LAST] = {
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC, //ucp_coll_COLL_BARRIER,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND, //ucp_coll_COLL_BCAST,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_REDUCE | UCP_GROUP_COLLECTIVE_MODIFIER_TARGET_RANK, //ucp_coll_COLL_REDUCE,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_REDUCE | UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC, //ucp_coll_COLL_ALLREDUCE,
        //ucp_coll_COLL_ALLREDUCE_SCATTER,
        UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_SOURCE, //ucp_coll_COLL_SCATTER,
        UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_SOURCE | UCP_GROUP_COLLECTIVE_MODIFIER_VECTOR_DATA, //ucp_coll_COLL_SCATTERV,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION | UCP_GROUP_COLLECTIVE_MODIFIER_TARGET_RANK, //ucp_coll_COLL_GATHER,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION | UCP_GROUP_COLLECTIVE_MODIFIER_VECTOR_DATA | UCP_GROUP_COLLECTIVE_MODIFIER_TARGET_RANK, //ucp_coll_COLL_GATHERV,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION | UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC, //ucp_coll_COLL_ALLGATHER,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION | UCP_GROUP_COLLECTIVE_MODIFIER_VECTOR_DATA | UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC, //ucp_coll_COLL_ALLGATHERV,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_SOURCE | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION | UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC, //ucp_coll_COLL_ALLTOALL,
        UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND | UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_SOURCE | UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION | UCP_GROUP_COLLECTIVE_MODIFIER_VECTOR_DATA | UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC, //ucp_coll_COLL_ALLTOALLV,
        //UCP_GROUP_COLLECTIVE_TYPE_LAST
};

static inline ucp_coll_step_maker_f ucp_coll_choose_director(ucp_coll_topo_map_t *map, ucp_group_collective_extended_modifiers_t flags)
{
    int one_req = (map->ep_cnt == 1) && (!UCP_COLL_TOPO_MAP_IS_MULTIROOT(map->flags));

    if (flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_TARGET_RANK)) {
        return one_req ? ucp_coll_direct_one_target : ucp_coll_direct_many_target;
    }

    return one_req ? ucp_coll_direct_one : ucp_coll_direct_many;
}

static inline ucs_status_t ucp_coll_init_map_instructions(ucp_coll_topo_map_t *map,
        int is_descent, int is_fragmented,
        ucs_list_link_t *instructions,
        ucp_group_collective_extended_modifiers_t coll_flags,
        ucp_coll_req_flags_t flags_master,
        ucp_coll_req_flags_t flags_slave)
{
    int one_req = (map->ep_cnt == 1) && (!UCP_COLL_TOPO_MAP_IS_MULTIROOT(map->flags));
    ucp_coll_step_maker_f director = ucp_coll_choose_director(map, coll_flags);
    ucp_coll_req_flags_t flags_used = (is_descent ^ (UCP_COLL_TOPO_MAP_IS_MASTER(map->flags) != 0)) ?
            flags_master : flags_slave;
    ucp_step_complete_cb_f step_cb;

    if (!is_descent) {
        flags_used |= UCS_BIT(UCP_COLL_REQ_FLAG_SOURCE);
    }

    if ((UCP_COLL_REQ_IS_RECV(flags_used)) && (UCP_COLL_REQ_IS_REDUCE(flags_used)) &&
            ((!one_req) || !is_descent)) {

        step_cb = one_req ?
                (is_fragmented ? (ucp_step_complete_cb_f)ucp_coll_comp_reduce_single_src_fragmented_complete_cb :
                        (ucp_step_complete_cb_f)ucp_coll_comp_reduce_single_src_contig_complete_cb) :
                        (is_descent ? (is_fragmented ? (ucp_step_complete_cb_f)ucp_coll_comp_reduce_dst_fragmented_complete_cb :
                                (ucp_step_complete_cb_f)ucp_coll_comp_reduce_dst_contig_complete_cb) :
                                (is_fragmented ? (ucp_step_complete_cb_f)ucp_coll_comp_reduce_src_fragmented_complete_cb :
                                        (ucp_step_complete_cb_f)ucp_coll_comp_reduce_src_contig_complete_cb));
    } else {
        step_cb = one_req ?
                (ucp_step_complete_cb_f)ucp_coll_comp_single_complete_cb :
                (ucp_step_complete_cb_f)ucp_coll_comp_bitfield_complete_cb;
    }

    return ucp_coll_common_instructions_add(instructions, map,
            director, step_cb, flags_used);
}

ucs_status_t ucp_coll_init_instructions(ucp_coll_topo_tree_t *tree,
		enum ucp_group_collective_type coll_type, int is_fragmented)
{
    unsigned map_idx;
    ucs_status_t error;
    ucp_coll_topo_map_t *next_map;
    ucp_coll_req_flags_t flags_master, flags_slave;

    ucp_group_collective_extended_modifiers_t coll_flags = coll_construct_flags[coll_type];
    ucs_list_link_t *instructions = &tree->instructions[coll_type];
    if (is_fragmented) {
        instructions += UCP_GROUP_COLLECTIVE_TYPE_LAST;
    }
    ucs_list_head_init(instructions);

    flags_slave = 0;
    flags_master = UCS_BIT(UCP_COLL_REQ_FLAG_RECV);

    if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_REDUCE)) {
        flags_master |= UCS_BIT(UCP_COLL_REQ_FLAG_REDUCE);
    } else {
        if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_SOURCE)) {
            flags_slave |= UCS_BIT(UCP_COLL_REQ_FLAG_SPLIT);
        }

        if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION)) {
            flags_master |= UCS_BIT(UCP_COLL_REQ_FLAG_SPLIT);
        }
    }

    if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_NEIGHBOR)) {
        next_map = &tree->maps[tree->map_count + UCP_COLL_TOPO_EXTRA_MAP_NEIGHBOR];

        error = ucp_coll_init_map_instructions(next_map, 0, is_fragmented,
                instructions, coll_flags, flags_master, flags_slave);
        if (error != UCS_OK) {
            goto destroy_traversal;
        }

        error = ucp_coll_init_map_instructions(next_map, 1, is_fragmented,
                instructions, coll_flags, flags_master, flags_slave);
        if (error != UCS_OK) {
            goto destroy_traversal;
        }
    }

    // create instructions for ascending the tree
    if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND)) {
        for (map_idx = 0; map_idx < tree->fanout_map; map_idx++) {
            next_map = &tree->maps[map_idx];

            error = ucp_coll_init_map_instructions (next_map, 0, is_fragmented,
                    instructions, coll_flags, flags_master, flags_slave);
            if (error != UCS_OK) {
                goto destroy_traversal;
            }
        }
    }

    // special case - upper layer sends to a particular rank
    if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_TARGET_RANK)) {
        next_map = &tree->maps[tree->map_count + UCP_COLL_TOPO_EXTRA_MAP_TARGET];

        error = ucp_coll_init_map_instructions (next_map, 0, is_fragmented,
                instructions, coll_flags, flags_master, flags_slave);
        if (error != UCS_OK) {
            goto destroy_traversal;
        }

        return UCS_OK;
    }

    // create instructions for descending the tree
    if (coll_flags & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND)) {
        for (map_idx = tree->fanout_map; map_idx < tree->map_count; map_idx++) {
            next_map = &tree->maps[map_idx];
            if (map_idx == tree->fanout_map) {
                if (UCP_COLL_TOPO_MAP_IS_MULTIROOT(next_map->flags)) {
                    flags_master |= UCS_BIT(UCP_COLL_REQ_FLAG_MULTIROOT);
                } else {
                    flags_master &= ~UCS_BIT(UCP_COLL_REQ_FLAG_REDUCE);
                }
            }

            error = ucp_coll_init_map_instructions (next_map, 1, is_fragmented,
                    instructions, coll_flags, flags_master, flags_slave);
            if (error != UCS_OK) {
                goto destroy_traversal;
            }

            flags_master &= ~UCS_BIT(UCP_COLL_REQ_FLAG_REDUCE);
            flags_master &= ~UCS_BIT(UCP_COLL_REQ_FLAG_MULTIROOT);
        }
    }

    return UCS_OK;

destroy_traversal:
    ucp_coll_destroy_traversal(instructions);
    return error;
}

ucs_status_t ucp_coll_init_tree_ops(ucp_coll_topo_tree_t *tree)
{
	enum ucp_group_collective_type coll_type = 0;
    ucs_status_t error;
    do {
        error = ucp_coll_init_instructions(tree, coll_type, 0);
        if (error == UCS_OK) {
        	error = ucp_coll_init_instructions(tree, coll_type, 1);
        }
        coll_type++;
    } while ((error == UCS_OK) && (coll_type < UCP_GROUP_COLLECTIVE_TYPE_LAST));
    return error;
}

void ucp_coll_cleanup_tree_ops(ucp_coll_topo_tree_t *tree)
{
	enum ucp_group_collective_type coll_type = 0;
    while (coll_type < UCP_GROUP_COLLECTIVE_TYPE_LAST) {
        ucp_coll_destroy_traversal(&tree->instructions[coll_type]);
        ucp_coll_destroy_traversal(&tree->instructions[coll_type + UCP_GROUP_COLLECTIVE_TYPE_LAST]);
        coll_type++;
    }
}

static inline int ucp_coll_is_hash_match(ucp_group_collective_params_t *req,
										 ucp_group_collective_params_t *ref)
{
	// TODO: optimize!
    return ((ref->sbuf == req->sbuf) &&
            (0 == memcmp(ref, req, offsetof(ucp_group_collective_params_t, cb))));
}

ucs_status_t ucp_coll_op_get(ucp_group_collective_params_t *params,
		ucs_list_link_t *op_cache, ucp_coll_op_t **op)
{
    ucp_coll_op_t *next_op;
    ucs_list_for_each(next_op, op_cache, cache) {
        if (ucp_coll_is_hash_match(params, &next_op->coll_params)) {
            next_op->coll_params.cb = params->cb;
            ucs_list_del(&next_op->cache);
            *op = next_op;
            return UCS_OK;
        }
    }
    return UCS_ERR_NO_ELEM;
}

ucs_status_ptr_t ucp_coll_op_run(ucp_coll_op_t *op)
{
	ucp_coll_step_t *step =
			ucs_queue_head_elem_non_empty(&op->steps,
					ucp_coll_step_t, queue);

	ucs_status_t error = ucp_coll_step_launch(step);
	if (error != UCS_OK) {
		return UCS_STATUS_PTR(error);
	}

	ucs_list_insert_before(&op->group->ops, &op->queue);
	return op; // TODO: return waitable/testable object
}

ucs_status_t ucp_coll_op_new(ucp_coll_topo_tree_t *tree,
                             const ucp_group_collective_params_t *coll_params,
							 ucp_coll_req_params_t *req_params,
							 ucp_coll_group_t *group,
							 ucp_coll_op_t **op)
{
    ucs_status_t error;
    ucp_coll_step_t *next_step = NULL;
    ucp_coll_instruction_t *instruction;

    ucp_coll_op_t *new_op = UCS_ALLOC_CHECK(sizeof(*new_op), "operation");

    /* select instructions to instantiate this collective */
    ucs_list_link_t *instructions = &tree->instructions[coll_params->type];
    ucs_assert(instructions != NULL);
    ucs_assert(!ucs_list_is_empty(instructions));
    ucs_queue_head_init(&new_op->steps);

    memcpy(&new_op->coll_params, coll_params, sizeof(*coll_params));
    new_op->sync_group = (coll_construct_flags[coll_params->type] & UCS_BIT(UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC)) ? group : NULL;

    /* turn instructions into steps */
    ucs_list_for_each(instruction, instructions, list) {
    	req_params->map = instruction->map;
    	req_params->flags = instruction->flags;
    	req_params->internal_cb = instruction->comp_cb;

        error = instruction->make_step(req_params, &next_step);
        if (error != UCS_OK) {
            ucp_coll_op_destroy(new_op);
            return error;
        }

        ucs_queue_push(&new_op->steps, &next_step->queue);
        next_step->del_op = NULL;
    }

    next_step->del_op = (void*)new_op;
    *op = new_op;
    return UCS_OK;
}

void ucp_coll_op_del(ucp_coll_op_t *op)
{
    /* memory cleanup */
    while (!ucs_queue_is_empty(&op->steps)) {
        ucp_coll_step_destroy(ucs_queue_pull_elem_non_empty(&op->steps,
                ucp_coll_step_t, queue));
    }
    ucs_free(op);
}

void ucp_coll_op_fin(void *op_ptr)
{
    ucp_coll_op_t *op = (ucp_coll_op_t*)op_ptr;

    // complete sync-ops (e.g. barrier, allreduce)
    if (ucs_unlikely(op->sync_group != NULL)) {
        ucp_coll_group_barrier_op_cb(op->sync_group, op);
    }

    // call the user-callback function (mark as complete)
    if (op->req_params.external_cb) {
    	op->req_params.external_cb(op, UCS_OK);
    }
}
