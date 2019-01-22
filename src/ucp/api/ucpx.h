/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2018.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCPX_H_
#define UCPX_H_

#include <ucp/api/ucp_def.h>
#include <ucs/sys/compiler_def.h>

/*
 * This header file is for experimental UCP API.
 * APIs defined here are NOT stable and may be removed / changed without notice.
 * By default, this header file is not installed. In order to install it, need
 * to run ./configure --enable-experimental-api
 */

BEGIN_C_DECLS

/* Needed for UCG - tentative prototype for the PoC ... */
void ucp_tag_recv_common_wrapper(ucp_worker_h worker, void *buffer, size_t count,
        uintptr_t datatype, ucp_tag_t tag, ucp_tag_t tag_mask,
        void *req, uint16_t req_flags, ucp_tag_recv_callback_t cb,
        void *rdesc, const char *debug_name);
ucs_status_t ucp_tag_send_req_wrapper(void *req, size_t dt_count,
        const void* msg_config, size_t rndv_rma_thresh,
        size_t rndv_am_thresh, ucp_send_callback_t cb, const void *proto,
        int enable_zcopy);

typedef ucs_status_t (*ucp_context_component_init)   (void **component_ctx);
typedef void         (*ucp_context_component_cleanup)(void  *component_ctx);
void ucp_context_register_ucg(ucp_context_component_init init,
                              ucp_context_component_cleanup cleanup);
void* ucp_worker_get_groups_ctx(ucp_worker_h worker);
ucs_status_t ucg_collective_req_init(void *op, ucp_worker_h worker, void *cb, void **req);


END_C_DECLS

#endif
