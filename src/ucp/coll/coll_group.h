#ifndef UCP_COLL_GROUP_H_
#define UCP_COLL_GROUP_H_

#include <ucs/datastruct/queue_types.h>
#include <ucs/datastruct/list_types.h>
#include <ucp/core/ucp_types.h>

#include "coll_ops.h"

typedef struct ucp_groups {
    ucp_coll_group_id_t next_id;
    ucs_list_link_t head;
} ucp_groups_t;

/* Exported functions for the Worker */
ucs_status_t ucp_worker_groups_init(ucp_groups_t *groups_ctx);
void ucp_worker_groups_cleanup(ucp_groups_t *groups_ctx);

/* Recycles the collective operation upon completion */
void ucp_coll_group_recycle_op(ucp_group_h group, ucp_coll_op_t *op);

#endif /* UCP_COLL_GROUP_H_ */
