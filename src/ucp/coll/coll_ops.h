#ifndef COLL_OPS_H_
#define COLL_OPS_H_

/*
 * Collective operations are composed of one or more steps.
 * In each step, we apply a method to a subgroup of peer processes.
 * Collectives are planned using "templates", and once the user
 * provides the details a step is "instantiated" from a suitable
 * template and the instance is executed. Often more than one instance
 * is created from the same template, and instances can run side-by-side.
 *
 * Methods are the basic algorithmic building blocks, like fan-in and
 * fan-out for trees, or the "Recursive K-ing" algorithm.
 * For example, Allreduce can either be done in two step,
 * fan-in and fanout, or in a single Recursive K-ing step.
 * Once the user requests an Allreduce operation - the selected
 * step templates are used to generate an instance
 * (or it is fetched from cache) and that instance is executed.
 */

#include <ucp/core/ucp_types.h>
#include <ucs/datastruct/list_types.h>
#include <ucs/datastruct/queue_types.h>

#include "coll_topo.h"

typedef void (*mpi_reduce_f)(void *mpi_op, void *src_buffer,
	void *dst_buffer, unsigned dcount, void* mpi_datatype);
extern mpi_reduce_f mpi_reduce;

enum ucp_coll_step_flags {
	UCP_COLL_STEP_FLAG_LENGTH_PER_REQUEST = UCS_BIT(0),
};


typedef struct ucp_coll_step_req ucp_coll_step_req_t;
struct ucp_coll_op;
struct ucp_coll_step {
	/**
	 * This part is used during the execution of this step.
	 */
    unsigned long long             pending;  /* completion bit-field (for resends) */
    char                          *reduced;  /* temporary reduction/aggregation storage */

	/**
	 * This part is set once, based on the collective operation parameters.
	 */
    struct ucp_coll_op            *op;       /* parent operation this step belongs to */
	ucs_queue_elem_t               queue;    /* pointer to the next step in the queue */
	unsigned                       flags;    /* modifiers for this step: enum ucp_coll_step_flags */

    /**
     * This is a special usage of callback arguments:
     * each step has an array of pointers, all pointing back to it, passed as arguments to the
     * callback functions for each individual send/recv request. This pointer implicitly contains
     * the index of each such send/recv request: the difference between the pointer and the "reqs".
     * This allows the callback function to set the corresponding bit in "pending", for example.
     */
	unsigned                       send_req_cnt;
	unsigned                       recv_req_cnt;
	ucp_coll_step_req_t           *reqs;
};


//typedef ucp_coll_req_t ucp_coll_op_hash_t;
typedef void(*ucc_op_complete_cb_f)(void *complete_cb_arg);

struct ucp_coll_op {
    ucs_queue_elem_t              queue;      /* part of per-group queue */
    ucs_queue_head_t              step_q;     /* queue of directives to follow */
    ucp_group_h                   group;      /* the group this operation belongs to */
    void                         *cb_req;     /* the request of this operation (for cb) */
    ucs_list_link_t               cache_list; /* cache list member */
    ucp_worker_h                  worker;     /* group's worker handle */
    ucp_group_collective_params_t params;     /* copy of the original call */
    unsigned                      step_cnt;   /* number of steps */
    struct ucp_coll_step          steps[];

    /*
     * In terms of memory layout, after the array of steps is the array of structures
     * used for callback for any of those steps:

    ucp_coll_step_req_t           cb_reqs[];
    */
};


ucs_status_t ucp_coll_step_init(ucp_coll_topo_phase_t *phs,
		  					    ucp_group_collective_params_t *params,
							    ucp_coll_step_t *step,
							    ucp_coll_step_req_t **cb_ptr);

void ucp_coll_step_set_tag(ucp_coll_step_t *step, ucp_tag_t tag);

ucs_status_t ucp_coll_step_execute(ucp_coll_step_t *step);

ucs_status_t ucp_coll_op_create(ucp_worker_h worker, ucp_coll_topo_t *topo,
		ucp_coll_group_id_t group_id, ucp_group_collective_params_t *params,
		ucp_coll_op_t **new_op);

ucs_status_t ucp_coll_op_recycle(ucp_coll_op_t *op);

void ucp_coll_op_destroy(ucp_coll_op_t *op);

#endif /* COLL_OPS_H_ */
