#ifndef UCP_COLL_H_
#define UCP_COLL_H_

#include "ucp/api/ucp.h"

#define UCP_COLL_PARAMS_DTYPE(_flags, _sbuf, _rbuf, _count, _dtype,        \
							  _mpi_op, _mpi_dtype, _group, _root, _cb) {   \
			.flags = _flags,                                               \
			.sbuf = _sbuf,                                                 \
			.rbuf = _rbuf,                                                 \
			.count = _count,                                               \
			.datatype = _dtype,                                            \
			.cb_r_op = _mpi_op,                                            \
			.cb_r_dtype = _mpi_dtype,                                      \
			.root = _root,                                                 \
			.comp_cb = cb                                                  \
}

static inline ucs_status_t ucp_coll_allreduce_init(const void *sbuf,
		void *rbuf, int count, ucp_datatype_t dtype, void *mpi_op,
		void *mpi_dtype, ucp_group_h group,
		enum ucp_group_collective_modifiers extra_flags,
	    ucp_group_collective_callback_t cb,
		ucp_coll_h *coll_p)
{
	ucp_group_collective_params_t params = UCP_COLL_PARAMS_DTYPE(
			UCP_GROUP_COLLECTIVE_MODIFIER_AGGREGATE | UCP_GROUP_COLLECTIVE_MODIFIER_BROADCAST,
			sbuf, rbuf, count, dtype, mpi_op, mpi_dtype, group, 0, cb);

	return ucp_group_collective_create(group, &params, coll_p);
}

static inline ucs_status_t ucp_coll_reduce_init(const void *sbuf,
		void *rbuf, int count, ucp_datatype_t dtype, void *mpi_op,
		void *mpi_dtype, ucp_group_h group, int root,
		enum ucp_group_collective_modifiers extra_flags,
	    ucp_group_collective_callback_t cb,
		ucp_coll_h *coll_p)
{
	ucp_group_collective_params_t params = UCP_COLL_PARAMS_DTYPE(
			UCP_GROUP_COLLECTIVE_MODIFIER_AGGREGATE,
			sbuf, rbuf, count, dtype, mpi_op, mpi_dtype, group, root, cb);

	return ucp_group_collective_create(group, &params, coll_p);
}

static inline ucs_status_t ucp_coll_bcast_init(const void *sbuf,
		void *rbuf, int count, ucp_datatype_t dtype,
		void *mpi_dtype, ucp_group_h group, int root,
		enum ucp_group_collective_modifiers extra_flags,
	    ucp_group_collective_callback_t cb,
		ucp_coll_h *coll_p)
{
	ucp_group_collective_params_t params = UCP_COLL_PARAMS_DTYPE(
			UCP_GROUP_COLLECTIVE_MODIFIER_BROADCAST,
			sbuf, rbuf, count, dtype, 0, mpi_dtype, group, root, cb);

	return ucp_group_collective_create(group, &params, coll_p);
}

static inline ucs_status_t ucp_coll_barrier_init(ucp_group_h group,
		enum ucp_group_collective_modifiers extra_flags,
	    ucp_group_collective_callback_t cb,
		ucp_coll_h *coll_p)
{
	ucp_group_collective_params_t params = {
			.flags = UCP_GROUP_COLLECTIVE_MODIFIER_AGGREGATE | UCP_GROUP_COLLECTIVE_MODIFIER_BROADCAST,
			.count = 0,
			.comp_cb = cb
	};

	return ucp_group_collective_create(group, &params, coll_p);
}

#endif /* UCP_COLL_H_ */
