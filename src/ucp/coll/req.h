/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2012.  ALL RIGHTS RESERVED.
* This software product is a proprietary product of Mellanox Technologies Ltd.
* (the "Company") and all right, title, and interest and to the software product,
* including all associated intellectual property rights, are and shall
* remain exclusively with the Company.
*
* This software product is governed by the End User License Agreement
* provided with the software product.
* $COPYRIGHT$
* $HEADER$
*/

#ifndef COLL_REQ_H_
#define COLL_REQ_H_

#include <ucp/api/ucp.h>
#include <uct/api/uct.h>

typedef union {
    struct {
        uint16_t coll_id;
        uint16_t group_id;
    };
    uct_tag_t full;
} ucp_coll_req_tag_t;

typedef enum ucp_coll_req_flags {
    UCP_COLL_REQ_FLAG_RECV = 0,
    UCP_COLL_REQ_FLAG_SOURCE,
    UCP_COLL_REQ_FLAG_SPLIT,
    UCP_COLL_REQ_FLAG_REDUCE,
    UCP_COLL_REQ_FLAG_MULTIROOT,
} ucp_coll_req_flags_t;

#define UCP_COLL_REQ_IS_RECV(flags)      ( flags & UCS_BIT(UCP_COLL_REQ_FLAG_RECV))
#define UCP_COLL_REQ_IS_SEND(flags)      (~flags & UCS_BIT(UCP_COLL_REQ_FLAG_RECV))
#define UCP_COLL_REQ_USE_SOURCE(flags)   ( flags & UCS_BIT(UCP_COLL_REQ_FLAG_SOURCE))
#define UCP_COLL_REQ_USE_DEST(flags)     (~flags & UCS_BIT(UCP_COLL_REQ_FLAG_SOURCE))
#define UCP_COLL_REQ_IS_SPLIT(flags)     ( flags & UCS_BIT(UCP_COLL_REQ_FLAG_SPLIT))
#define UCP_COLL_REQ_IS_REDUCE(flags)    ( flags & UCS_BIT(UCP_COLL_REQ_FLAG_REDUCE))
#define UCP_COLL_REQ_IS_MULTIROOT(flags) ( flags & UCS_BIT(UCP_COLL_REQ_FLAG_MULTIROOT))

/* Step completion callback prototype */
typedef struct ucp_coll_req ucp_coll_req_t;
typedef struct ucp_coll_step ucp_coll_step_t;
typedef void(*ucp_step_complete_cb_f)(ucp_coll_step_t **step_cb_arg);

typedef struct ucp_coll_topo_map ucp_coll_topo_map_t;
typedef struct ucp_coll_topo_tree ucp_coll_topo_tree_t;
typedef struct ucp_coll_req_params {
	/* User-specified */
	ucp_group_create_params_t     *group_params;
    ucp_group_collective_params_t *coll_params;

    /* Generated */
    ucp_coll_req_flags_t           flags;
    ucp_coll_topo_tree_t          *tree;
    ucp_coll_topo_map_t           *map;
    ucp_coll_req_tag_t             tag;
    ucp_coll_proc_h                root;

    union {
    	ucp_step_complete_cb_f          internal_cb; /* Next step to launch upon completion */
        ucp_group_collective_callback_t external_cb; /* External function to be called upon completion */
    };
} ucp_coll_req_params_t;

enum ucp_coll_req_method_type {
    UCP_COLL_METHOD_SEND_TM_EAGER_SHORT,
    UCP_COLL_METHOD_SEND_TM_EAGER_BCOPY,
    UCP_COLL_METHOD_SEND_TM_EAGER_ZCOPY,
    UCP_COLL_METHOD_SEND_TM_RNDV_ZCOPY,
    UCP_COLL_METHOD_RECV_TM_ZCOPY,
};

union ucp_coll_req_method_func {
    ucs_status_t    (*ep_tag_eager_short)(uct_ep_h ep, uct_tag_t tag,
                                          const void *data, size_t length);
    ssize_t         (*ep_tag_eager_bcopy)(uct_ep_h ep, uct_tag_t tag, uint64_t imm,
                                          uct_pack_callback_t pack_cb, void *arg);
    ucs_status_t    (*ep_tag_eager_zcopy)(uct_ep_h ep, uct_tag_t tag, uint64_t imm,
                                          const uct_iov_t *iov, size_t iovcnt,
                                          uct_completion_t *comp);
    ucs_status_ptr_t (*ep_tag_rndv_zcopy)(uct_ep_h ep, uct_tag_t tag,
                                          const void *header,
                                          unsigned header_length,
                                          const uct_iov_t *iov,
                                          size_t iovcnt,
                                          uct_completion_t *comp);
    ucs_status_t  (*iface_tag_recv_zcopy)(uct_iface_h iface, uct_tag_t tag,
                                          uct_tag_t tag_mask,
                                          const uct_iov_t *iov,
                                          size_t iovcnt,
                                          uct_tag_context_t *ctx);
};

struct ucp_coll_req{
    ucp_group_collective_params_t *params; /* For handling completion */
	enum ucp_coll_req_method_type  method; /* UCT function selector */
	union ucp_coll_req_method_func func;   /* UCT function pointer */
	union {                                /* UCT base entity */
		uct_ep_h            ep;
		uct_iface_h         iface;
	};

	/* Additional parameters for UCT request */
	uct_tag_t               tag;
	void                   *data;
	size_t                  length;
	uint64_t                imm;
	union {
		uct_iov_t          *iov;
		uct_pack_callback_t pack_cb;
	};
	union {
		size_t              iovcnt;
		void               *arg;
	};
	union {
		uct_completion_t   *comp;
		uct_tag_context_t  *ctx;
	};
};

ucs_status_t ucp_coll_req_fill (ucp_coll_req_params_t *params,
								  ucp_coll_req_t *reqs);
ucs_status_t ucp_coll_req_trigger(ucp_coll_req_t *req);
void         ucp_coll_req_destroy(ucp_coll_req_t *reqs);

#endif /* COLL_TL_H_ */
