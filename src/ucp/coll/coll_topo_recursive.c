#include "coll_topo.h"

#include <string.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack.h>

ucs_status_t ucp_coll_topo_recursive_create(struct ucp_coll_topo_params *params, ucp_coll_topo_t **topo_p)
{
    /* Calculate the number of recursive steps */
    unsigned proc_count = params->group_params->member_count;
    unsigned factor = params->recursive_factor;
    unsigned step_idx = 0, step_size = 1;
    if (factor < 2) {
        ucs_error("Recursive K-ing factor must be at least 2 (given %u)", factor);
        return UCS_ERR_INVALID_PARAM;
    }
    while (step_size < proc_count) {
        step_size *= factor;
        step_idx++;
    }
    if (step_size != proc_count) {
        ucs_error("Recursive K-ing must have proc# a power of the factor (factor %u procs %u)", factor, proc_count);
        /* Currently only an exact power of the recursive factor is supported */
        return UCS_ERR_UNSUPPORTED;
    }

    /* Allocate memory resources */
    size_t alloc_size = sizeof(ucp_coll_topo_t) +
            step_idx * sizeof(ucp_coll_topo_phase_t);
    if (factor != 2) {
        /* Allocate extra space for the map's multiple endpoints */
        alloc_size += step_idx * (factor - 1) * sizeof(uct_ep_h);
    }
    struct ucp_coll_topo *recursive = (struct ucp_coll_topo*)UCS_ALLOC_CHECK(alloc_size, "recursive topology");
    ucp_coll_topo_phase_t *phase    = &recursive->phss[0];
    ucp_ep_h *next_ep               = (ucp_ep_h*)(phase + step_idx);
    recursive->phs_cnt              = step_idx;
    recursive->ep_cnt               = step_idx * 2 * (factor - 1);

    /* Find my own index */
    ucp_group_member_index_t my_index = 0;
    while ((my_index < proc_count) &&
           (params->group_params->distance[my_index] !=
                   UCP_GROUP_MEMBER_DISTANCE_SELF)) {
        my_index++;
    }

    if (my_index == proc_count) {
        ucs_error("No member with distance==UCP_GROUP_MEMBER_DISTANCE_SELF found");
        return UCS_ERR_INVALID_PARAM;
    }

    /* Calculate the peers for each step */
    for (step_idx = 0, step_size = 1;
         step_idx < recursive->phs_cnt;
         step_idx++, phase++, step_size *= params->recursive_factor) {
        unsigned step_base = my_index - (my_index % (step_size * factor));
        if (factor == 2) {
            phase->ep_cnt = 1;
            next_ep = &phase->single_ep;
        } else {
            phase->ep_cnt = factor - 1;
        }
        phase->method = UCP_COLL_TOPO_METHOD_REDUCE_RECURSIVE;

        /* In each step, there are one or more peers */
        unsigned step_peer_idx;
        for (step_peer_idx = 1;
             step_peer_idx < params->recursive_factor;
             step_peer_idx++, next_ep++) {
            unsigned peer_index = step_base +
                    ((my_index - step_base + step_size * step_peer_idx) %
                     (step_size * factor));
            if (ucp_coll_topo_connect(params->group, peer_index, next_ep)) {
                memset(next_ep, 0, alloc_size - ((char*)next_ep - (char*)recursive));
                ucp_coll_topo_destroy(recursive);
                return UCS_ERR_UNREACHABLE;
            }
        }
    }

    *topo_p = recursive;
    return UCS_OK;
}
