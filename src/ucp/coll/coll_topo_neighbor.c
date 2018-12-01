#include "coll_topo.h"

#include <string.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack.h>

#define UCP_COLL_TOPO_CALC_CELL_UP(me, sqrt_total, total)                                   \
       ((me + total - sqrt_total) % total)
#define UCP_COLL_TOPO_CALC_CELL_DOWN(me, sqrt_total, total)                                 \
       ((me + sqrt_total) % total)
#define UCP_COLL_TOPO_CALC_CELL_LEFT(me, sqrt_total, total)                                 \
       ((me % sqrt_total) ? (my_rank + total - 1) % total : me + total)
#define UCP_COLL_TOPO_CALC_CELL_RIGHT(me, sqrt_total, total)                                \
       (((me + 1) % sqrt_total) ? (me + 1) % total : (me + total - sqrt_total) % total)

#define ucp_coll_topo_connect_CELL(dir, params, my_rank, dim_size, rank_cnt, phs, ep_slot)  \
       ucp_coll_topo_connect((params)->group, UCP_COLL_TOPO_CALC_CELL##dir                  \
               ((my_rank), (dim_size), (rank_cnt)), &(phs)->multi_eps[(ep_slot)])

ucs_status_t ucp_coll_topo_neighbor_create(struct ucp_coll_topo_params *params, ucp_coll_topo_t **topo_p)
{
	/* Check against what's actually supported */
	unsigned proc_count = params->group_params->total_ranks;
	if (params->neighbor_dimension != 2) {
		ucs_error("One 2D neighbor collectives are supported.");
		return UCS_ERR_UNSUPPORTED;
	}

	/* Find the size of a single dimension */
	unsigned dim_size = 1;
	while (dim_size * dim_size < proc_count) {
		dim_size++;
	}

	/* Sanity check */
	if (dim_size * dim_size != proc_count) {
		ucs_error("Neighbor topology must have proc# a power of the dimension (dim %u procs %u)",
				params->neighbor_dimension, proc_count);
		return UCS_ERR_INVALID_PARAM;
	}

	/* Allocate memory resources */
	size_t alloc_size = sizeof(ucp_coll_topo_t) +
			sizeof(ucp_coll_topo_phase_t) + 4 * sizeof(uct_ep_h);
	struct ucp_coll_topo *neighbor =
			(struct ucp_coll_topo*)UCS_ALLOC_CHECK(alloc_size, "neighbor topology");

    ucp_group_rank_t my_rank       = params->group_params->my_rank_index;
    ucp_group_rank_t rank_cnt      = params->group_params->total_ranks;
    ucp_coll_topo_phase_t *nbr_phs = (ucp_coll_topo_phase_t*)(neighbor + 1);
    nbr_phs->multi_eps             = (ucp_ep_h*)(nbr_phs + 1);
    nbr_phs->method                = UCP_COLL_TOPO_METHOD_NEIGHBOR;
    nbr_phs->ep_cnt                = 4;
	neighbor->phs_cnt              = 1;

    ucs_status_t status;
    if (((status = ucp_coll_topo_connect_CELL(_UP,    params, my_rank, dim_size, rank_cnt, nbr_phs, 0)) == UCS_OK) ||
    	((status = ucp_coll_topo_connect_CELL(_DOWN,  params, my_rank, dim_size, rank_cnt, nbr_phs, 1)) == UCS_OK) ||
		((status = ucp_coll_topo_connect_CELL(_LEFT,  params, my_rank, dim_size, rank_cnt, nbr_phs, 2)) == UCS_OK) ||
		((status = ucp_coll_topo_connect_CELL(_RIGHT, params, my_rank, dim_size, rank_cnt, nbr_phs, 3)) == UCS_OK)) {
    	*topo_p = neighbor;
    	return UCS_OK;
    }

	return status;
}
