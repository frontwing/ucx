#include <ucs/debug/memtrack.h>

#include "coll_topo.h"

ucs_status_t ucp_coll_topo_create(struct ucp_coll_topo_params *params,
		ucp_coll_topo_t **topo_p)
{
	switch(params->type) {
	case UCP_COLL_TOPO_RECURSIVE:
		return ucp_coll_topo_recursive_create(params, topo_p);

	default:
		return ucp_coll_topo_tree_create(params, topo_p);
	}

	return UCS_ERR_UNSUPPORTED;
}

void ucp_coll_topo_destroy(struct ucp_coll_topo *topo)
{
	unsigned phs_idx, ep_idx;
	ucp_coll_topo_phase_t *phase = &topo->phss[0];
	for (phs_idx = 0; phs_idx < topo->phs_cnt; phs_idx++, phase++) {
		ucp_ep_h *eps = UCP_COLL_GET_EPS(phase);
		for (ep_idx = 0; ep_idx < phase->ep_cnt; ep_idx++, eps++) {
			ucp_ep_destroy(*eps);
		}
	}
	ucs_free(topo);
}
