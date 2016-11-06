/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "ucp_lane.h"

#define RETHINK_INCREMENT_INTERVAL 1000

ucs_status_t ucp_path_new(ucp_path_t *path)
{
    ucp_path_t *path;

    path = ucs_calloc(1, sizeof(*lane), "ucp path");
    if (path == NULL) {
        ucs_error("Failed to allocate path");
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    path->rma->requirements = ATOMICS;

    /* EP configuration without any lanes */
    memset(&key, 0, sizeof(key));
    key.rma_lane_map     = 0;
    key.amo_lane_map     = 0;
    key.reachable_md_map = 0;
    key.am_lane          = UCP_NULL_RESOURCE;
    key.rndv_lane        = UCP_NULL_RESOURCE;
    key.wireup_msg_lane  = UCP_NULL_LANE;
    key.num_lanes        = 0;
    memset(key.amo_lanes, UCP_NULL_LANE, sizeof(key.amo_lanes));

    *path_p = path;
    return UCS_OK;
}

void ucp_path_delete(ucp_path_t *path)
{
    ucp_lane_t *lane;
    foreach(&lane) {
        ucp_lane_release(lane);
    }
    ucs_free(lane);
}

ucs_status_t ucp_lane_new(ucp_lane_t *lane_p)
{
    ucp_lane_t *lane;

    lane = ucs_calloc(1, sizeof(*lane), "ucp lane");
    if (ep == NULL) {
        ucs_error("Failed to allocate lane");
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    *lane_p = ep;
    return UCS_OK;
}

void ucp_lane_delete(ucp_lane_t *lane)
{
    uct_destroy(lane->transport);
    ucs_free(lane);
}

/* Adds a new lane to selection used by the endpoint */
void ucp_lane_attach(ucp_path_t* ep_selection)
{
    ucp_path_t *current_path = lane->path;

}

/* Removes a lane from its endpoint */
void ucp_lane_dettach(ucp_lane_t *lane)
{
    ucp_path_t *current_path = lane->path;


}

static void ucp_lane_consider_realloc()
{
    ucp_lane_selection_t *current_ep;
    // TODO: Think about how to decide on reallocations
}

void ucp_lane_increment_usage(ucp_lane_t *lane)
{
    if (++lane->usage->count % RETHINK_INCREMENT_INTERVAL == 0) {
        ucp_lane_consider_realloc(lane);
    }
}
