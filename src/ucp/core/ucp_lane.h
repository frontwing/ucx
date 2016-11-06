/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */


#ifndef UCP_LANE_H_
#define UCP_LANE_H_

#include "ucp_context.h"

enum lane_purpose {
    LANE_PURPOSE_ACTIVE_MESSAGES = 0,
    LANE_PURPOSE_REMOTE_MEMORY_ACCESS,
    LANE_PURPOSE_ATOMIC_OPERATIONS,
    LANE_PURPOSE_RANDEVOUS,
    LANE_PURPOSE_WIREUP,

    LANE_PURPOSE_MAX
};

typedef struct ucp_lane_context {
    list_head_t lanes;
} ucp_lane_context_t;

typedef struct ucp_lane_usage {
    unsigned long count;
} ucp_lane_usage_t;

typedef struct ucp_lane {
    ucp_lane_usage_t usage;
    uct_tl_t transport; //for sync on attach/detach
    list_link_t list;

} ucp_lane_t;

typedef struct ucp_md_lane_subset {
    enum requirements;
    ucp_lane_index_t       lanes[UCP_MAX_LANES];
    /* Lane-map lookup for lanes:
     * Every group of UCP_MD_INDEX_BITS consecutive bits in the map (in total
     * UCP_MAX_LANES such groups) is a bitmap. All bits in it are zero, except
     * the md_index-th bit of that lane. If the lane is unused, all bits are zero.
     * For example, the bitmap '00000100' means the lane remote md_index is 2.
     * It allows to quickly lookup
     */
    ucp_md_lane_map_t      lane_map;
} ucp_md_lane_subset_t;

/* Lanes configuration.
 * Every lane is a UCT endpoint to the same remote worker.
 */
typedef struct ucp_path {
    ucp_md_lane_subset_t subset[LANE_PURPOSE_MAX];

    /* Bitmap of remote mds which are reachable from this endpoint (with any set
     * of transports which could be selected in the future)
     */
    ucp_md_map_t            reachable_md_map;
    ucp_rsc_index_t         lanes[UCP_MAX_LANES];/* Resource index for every lane */
    ucp_lane_index_t        num_lanes;           /* Number of lanes */
} ucp_path_t;

ucs_status_t ucp_lane_init(ucp_lane_context_t **ctx);

void ucp_lane_finalize(ucp_lane_context_t *ctx);

ucs_status_t ucp_lane_new(tl_channel ch, ucp_lane_t *lane);

void ucp_lane_delete(ucp_lane_t *lane);

ucs_status_t ucp_path_new(ucp_path_t *path);

void ucp_path_delete(ucp_path_t *path);

/* Adds a new lane to selection used by the endpoint */
void ucp_lane_attach(ucp_path_t* ep_selection);

/* Removes a lane from its endpoint */
void ucp_lane_dettach(ucp_lane_t *lane);

void ucp_lane_increment_usage(ucp_lane_t *lane);

#endif
