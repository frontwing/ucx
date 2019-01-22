/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifndef UCG_OPS_H_
#define UCG_OPS_H_

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

#include "types.h"

#include <ucg/topology/topo.h>
#include <ucs/datastruct/list_types.h>
#include <ucs/datastruct/queue_types.h>

typedef void (*mpi_reduce_f)(void *mpi_op, void *src_buffer,
        void *dst_buffer, unsigned dcount, void* mpi_datatype);
extern mpi_reduce_f mpi_reduce;

enum ucg_step_flags {
    UCG_STEP_FLAG_LENGTH_PER_REQUEST = UCS_BIT(0),
};


typedef struct ucg_step_req ucg_step_req_t;
struct ucg_op;
struct ucg_step {
    /**
     * This part is used during the execution of this step.
     */
    unsigned long long pending;  /* completion bit-field (for resends) */
    char              *reduced;  /* temporary reduction/aggregation storage */

    /**
     * This part is set once, based on the collective operation parameters.
     */
    struct ucg_op     *op;       /* parent operation this step belongs to */
    ucs_queue_elem_t   queue;    /* pointer to the next step in the queue */
    unsigned           flags;    /* modifiers for this step: enum ucg_step_flags */

    /**
     * This is a special usage of callback arguments:
     * each step has an array of pointers, all pointing back to it, passed as arguments to the
     * callback functions for each individual send/recv request. This pointer implicitly contains
     * the index of each such send/recv request: the difference between the pointer and the "reqs".
     * This allows the callback function to set the corresponding bit in "pending", for example.
     */
    unsigned           send_req_cnt;
    unsigned           recv_req_cnt;
    ucg_step_req_t    *reqs;
};

//typedef ucg_req_t ucg_op_hash_t;
typedef void(*ucg_op_complete_cb_f)(void *complete_cb_arg);

struct ucg_op {
    ucs_queue_elem_t        queue;      /* part of per-group queue */
    ucs_queue_head_t        step_q;     /* queue of directives to follow */
    ucg_group_h             group;      /* the group this operation belongs to */
    void                   *cb_req;     /* the request of this operation (for cb) */
    ucs_list_link_t         cache_list; /* cache list member */
    ucg_worker_h            worker;     /* group's worker handle */
    ucg_collective_params_t params;     /* copy of the original call */
    unsigned                step_cnt;   /* number of steps */
    struct ucg_step         steps[];

    /*
     * In terms of memory layout, after the array of steps is the array of structures
     * used for callback for any of those steps:

    ucg_step_req_t          cb_reqs[];
    */
};


ucs_status_t ucg_step_init(ucg_topo_phase_t *phs,
                           ucg_collective_params_t *params,
                           ucg_step_t *step,
                           ucg_step_req_t **cb_ptr);

void ucg_step_set_tag(ucg_step_t *step, ucp_tag_t tag);

ucs_status_t ucg_step_execute(ucg_step_t *step);

ucs_status_t ucg_op_create(ucg_worker_h worker, ucg_topo_t *topo,
        ucg_group_id_t group_id, ucg_collective_params_t *params,
        ucg_op_t **new_op);

ucs_status_t ucg_op_recycle(ucg_op_t *op);

void ucg_op_destroy(ucg_op_t *op);

#endif /* UCG_OPS_H_ */
