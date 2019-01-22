/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifndef UCG_GROUP_H_
#define UCG_GROUP_H_

#include <ucs/datastruct/queue_types.h>
#include <ucs/datastruct/list_types.h>
#include <ucs/datastruct/mpool.inl>
#include <ucp/core/ucp_types.h>

#include "ops.h"

typedef struct ucg_groups {
    ucg_group_id_t next_id;
    ucs_list_link_t head;
} ucg_groups_t;

/* Exported functions for the Worker */
//ucs_status_t ucg_worker_groups_init(ucg_groups_t **groups_ctx);
//void ucg_worker_groups_cleanup(ucg_groups_t *groups_ctx);

/* Recycles the collective operation upon completion */
void ucg_group_recycle_op(ucg_group_h group, ucg_op_t *op);

#endif /* UCG_GROUP_H_ */
