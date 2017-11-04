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

#ifndef COLL_OPS_H_
#define COLL_OPS_H_

#include "req.h"
#include "topo.h"
#include "step.h"

#include <ucs/datastruct/list.h>

typedef enum {
    UCP_GROUP_COLLECTIVE_MODIFIER_ASCEND = UCP_GROUP_COLLECTIVE_MODIFIER_LAST,
    UCP_GROUP_COLLECTIVE_MODIFIER_DESCEND,
    UCP_GROUP_COLLECTIVE_MODIFIER_REDUCE,
    UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_SOURCE,
    UCP_GROUP_COLLECTIVE_MODIFIER_SPLIT_DESTINATION,
    UCP_GROUP_COLLECTIVE_MODIFIER_VECTOR_DATA,
    UCP_GROUP_COLLECTIVE_MODIFIER_VECTOR_DATATYPE,
    UCP_GROUP_COLLECTIVE_MODIFIER_MEMSYNC,
        // TODO: consider replacing w/ (ascend&&descend)
} ucp_group_collective_extended_modifiers_t;

typedef uint32_t ucp_coll_id_t;

typedef ucs_status_t(*ucp_coll_step_maker_f)(ucp_coll_req_params_t *req_spec,
        ucp_coll_step_t **step_p);

typedef struct ucp_coll_group ucp_coll_group_t;
typedef struct ucp_coll_op ucp_coll_op_t;

struct ucp_coll_op {
	ucp_coll_group_t             *group;       /* group context the op belongs to */
	union {
		ucs_list_link_t           queue;       /* member of per-group ops queue */
		ucs_list_link_t           cache;       /* member of per-group ops cache */
	};
    ucs_queue_head_t              steps;       /* queue of steps to follow */
    ucp_group_collective_params_t coll_params; /* copy of the original collective request */
    ucp_coll_req_params_t         req_params;  /* information for the transport request */
    ucp_coll_group_t             *sync_group;  /* only for mem-sync ops */
} ;

typedef struct ucp_coll_instruction {
    ucs_list_link_t list;            /* member in a list of instructions */
    ucp_coll_topo_map_t *map;        /* map of peers to exchange data */
    ucp_coll_step_maker_f make_step; /* function to init transport requests */
    ucp_step_complete_cb_f comp_cb;  /* callback to use upon step completion */
    enum ucp_coll_req_flags flags;   /* flags to be passed to transport level */
} ucp_coll_instruction_t;

ucs_status_t ucp_coll_common_instructions_add(ucs_list_link_t *instructions_head,
        ucp_coll_topo_map_t* map, ucp_coll_step_maker_f direct,
        ucp_step_complete_cb_f callback, ucp_coll_req_flags_t flags);

void ucp_coll_destroy_traversal(ucs_list_link_t *instructions);


ucs_status_t     ucp_coll_op_get(ucp_group_collective_params_t *params,
                                 ucs_list_link_t *op_cache,
					    		 ucp_coll_op_t **op);
ucs_status_t     ucp_coll_op_new(ucp_coll_topo_tree_t *tree,
                                 const ucp_group_collective_params_t *coll_params,
			    				 ucp_coll_req_params_t *req_params,
				    			 ucp_coll_group_t *group,
					    		 ucp_coll_op_t **op);
ucs_status_ptr_t ucp_coll_op_run(ucp_coll_op_t *op);
void             ucp_coll_op_del(ucp_coll_op_t *op);
void             ucp_coll_op_fin(void *op);

ucs_status_t ucp_coll_init_tree_ops(ucp_coll_topo_tree_t *tree);
void         ucp_coll_cleanup_tree_ops(ucp_coll_topo_tree_t *tree);

#endif /* COLL_OPS_H_ */
