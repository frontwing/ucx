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


#ifndef COLL_STEP_H_
#define COLL_STEP_H_

#include <ucs/sys/sys.h>
#include <ucs/datastruct/queue.h>
#include <ucs/debug/memtrack.h>
#include <ucs/debug/log.h>

#include "topo.h"

enum ucp_coll_mt {
    UCP_COLL_MT_FANIN = 0,
    UCP_COLL_MT_FANOUT,
    UCP_COLL_MT_RECURSIVE_DOUBLING,
    UCP_COLL_MT_REDUCE,
    // TODO: UCP_COLL_MT_STABLE_REDUCE,

    UCP_COLL_MT_LAST
};

struct ucp_coll_step;
struct ucp_coll_step {
    ucs_queue_elem_t       queue;        /* member in the list of steps for an op */
    uint64_t               pending_mask; /* completion bit-field */
    ucp_coll_req_params_t *req_params;   /* request creation parameters */

    unsigned               num_req;      /* number of transport requests */
    ucp_coll_req_t        *reqs;         /* transport requests array */

    void                  *del_op;       /* optional pointer to operation for removal */
    uint8_t               *partial_data; /* optional pointer for stable aggregation */

    /*
     * Optimization:
     * pointers to step for transport call-back context.
     * Each callback is given a different cell as argument - to determine
     * which request was completed.
     */
    struct ucp_coll_step **step_cb_args;
};

#define UCP_COLL_STEP_PENDING_BITMASK(idx)      ()
#define UCP_COLL_STEP_IS_PENDING(step, idx)     (step->pending_mask & UCS_BIT(idx))
#define UCP_COLL_STEP_UPDATE_PENDING(step, idx) {step->pending_mask &= ~UCS_BIT(idx);}


/* Step launch prototype */
typedef ucs_status_t(*ucp_coll_step_start_f)(ucp_coll_req_params_t *req_spec,
        ucp_coll_step_t **step_p);

ucs_status_t ucp_coll_common_init_req(ucp_coll_step_t *step,
        ucp_coll_req_params_t *req_spec);
ucs_status_t ucp_coll_common_direct(ucp_coll_step_t *prev,
		ucp_coll_req_params_t *req_spec, ucp_coll_step_t **step_p);
//inline void ucp_coll_common_complete_cb(ucp_coll_step_t *step);

/* recycles ops on completing their last step - implemented elsewhere */
void ucp_coll_op_fin(void *op);

/* Method-specific functions */

ucs_status_t ucp_coll_step_create(ucp_coll_topo_map_t *map, ucp_coll_step_t **step);
ucs_status_t ucp_coll_step_launch(ucp_coll_step_t *step);
void ucp_coll_step_destroy(ucp_coll_step_t *step);

/* Exported callback function types */
size_t ucp_coll_comp_reduce_src_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);
size_t ucp_coll_comp_reduce_dst_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);
size_t ucp_coll_comp_reduce_single_src_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);
size_t ucp_coll_comp_reduce_single_dst_fragmented_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);

size_t ucp_coll_comp_reduce_src_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);
size_t ucp_coll_comp_reduce_dst_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);
size_t ucp_coll_comp_reduce_single_src_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);
size_t ucp_coll_comp_reduce_single_dst_contig_complete_cb(void *buffer,
        size_t length, size_t offset, ucp_coll_step_t **cb_dir);

void ucp_coll_comp_bitfield_complete_cb(ucp_coll_step_t **cb_dir);
void ucp_coll_comp_single_complete_cb(ucp_coll_step_t **cb_dir);


/* Exported step function types */

ucs_status_t ucp_coll_direct_one(
        ucp_coll_req_params_t *req_spec, ucp_coll_step_t **step_p);
ucs_status_t ucp_coll_direct_many(
        ucp_coll_req_params_t *req_spec, ucp_coll_step_t **step_p);

ucs_status_t ucp_coll_direct_one_target(
        ucp_coll_req_params_t *req_spec, ucp_coll_step_t **step_p);
ucs_status_t ucp_coll_direct_many_target(
        ucp_coll_req_params_t *req_spec, ucp_coll_step_t **step_p);

#endif /* COLL_STEP_H_ */
