/**
* Copyright (C) Mellanox Technologies Ltd. 2018.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

/*
 * UCP client - server example utility
 * -----------------------------------------------
 *
 * Server side:
 *
 *    ./ucp_client_server
 *
 * Client side:
 *
 *    ./ucp_client_server -a <server-ip>
 *
 * Notes:
 *
 *    - The server will listen to incoming connection requests on INADDR_ANY.
 *    - The client needs to pass the IP address of the server side to connect to,
 *      as the first and only argument to the test.
 *    - Currently, the passed IP needs to be an IPoIB or a RoCE address.
 *    - The amount of used resources (HCA's and transports) needs to be limited
 *      for this test (for example: UCX_NET_DEVICES=mlx5_0:1 UCX_TLS=rc_x).
 *      This is currently required since the UCP layer has a limitation on
 *      the size of the transfered transports addresses that are being passed
 *      to the remote peer.
 *      Therefore, the current usage should be, for example:
 *      Server side:
 *              UCX_NET_DEVICES=mlx5_0:1 UCX_TLS=rc ./ucp_client_server
 *      Client side:
 *              UCX_NET_DEVICES=mlx5_0:1 UCX_TLS=rc ./ucp_client_server -a <server-address>
 *
 */

#include <ucp/api/ucp.h>

#include <string.h>    /* memset */
#include <unistd.h>    /* getopt */
#include <stdlib.h>    /* atoi */
#include <mpi.h>       /* MPI_* */

enum ucp_group_collective_type collective_type = 0;

/**
 * Server context to be used in the user's accept callback.
 * It holds the server's endpoint which will be created upon accepting a
 * connection request from the client.
 */
typedef struct ucx_server_ctx {
    ucp_ep_h     ep;
} ucx_server_ctx_t;

/**
 * Print this application's usage help message.
 */
static void usage()
{
    fprintf(stderr, "Usage: mpirun [mpi-parameters] ucp_collective [collective-parameters]\n");
    fprintf(stderr, "UCP groups and collective operations example utility\n");
    fprintf(stderr, "\nParameters are:\n");
    fprintf(stderr, " -c\tSet collective operation y (default:0 - Barrier)\n");
    fprintf(stderr, "\n");
}

/**
 * Parse the command line arguments.
 */
static int parse_cmd(int argc, char *const argv[], ucp_group_create_params_t *params)
{
    int c = 0;
    opterr = 0;

    while ((c = getopt(argc, argv, "c:")) != -1) {
        switch (c) {
        case 'c':
            collective_type = atoi(optarg);
            if (collective_type > UCP_GROUP_COLLECTIVE_TYPE_LAST) {
                fprintf(stderr, "Unknown collective type: %d\n", collective_type);
                return -1;
            }
            break;
        default:
            usage();
            return -1;
        }
    }

    return 0;
}

/**
 * Initialize the UCP context and worker.
 */
static int init_context(ucp_context_h *ucp_context, ucp_worker_h *ucp_worker)
{
    /* UCP objects */
    ucp_worker_params_t worker_params;
    ucp_params_t ucp_params;
    ucp_config_t *config;
    ucs_status_t status;
    int ret = 0;

    memset(&ucp_params, 0, sizeof(ucp_params));
    memset(&worker_params, 0, sizeof(worker_params));

    /* UCP initialization */
    status = ucp_config_read(NULL, NULL, &config);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_config_read (%s)\n", ucs_status_string(status));
        ret = -1;
        goto err;
    }

    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features   = UCP_FEATURE_COLL;

    status = ucp_init(&ucp_params, config, ucp_context);
    ucp_config_release(config);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_init (%s)\n", ucs_status_string(status));
        ret = -1;
        goto err;
    }

    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(*ucp_context, &worker_params, ucp_worker);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_worker_create (%s)\n", ucs_status_string(status));
        ret = -1;
        goto err_cleanup;
    }

    return ret;

err_cleanup:
    ucp_cleanup(*ucp_context);

err:
    return ret;
}

static void get_ucx_address_by_rank(unsinged rank, void *proc_data, void **ucx_address)
{
	*ucx_address = proc_data;
}

int main(int argc, char **argv)
{
    int i, ret;

    /* UCP objects */
    ucp_group_collective_params_t coll_params;
    ucp_group_create_params_t group_params;
    ucp_context_h ucp_context;
    ucp_worker_h ucp_worker;
    ucp_group_h ucp_group;
    ucp_coll_h ucp_coll;
    ucs_status_t status;

    MPI_Init(&argc, &argv);

    ret = parse_cmd(argc, argv, &group_params);
    if (ret != 0) {
        goto err;
    }

    /* Initialize the UCX required objects */
    ret = init_context(&ucp_context, &ucp_worker);
    if (ret != 0) {
        goto err;
    }

    MPI_Alltoallv(ucx_addresses); // TODO: get my UCX address, exchange using MPI_Alltoallv()

    group_params.tree.radix = 2; // TODO: cleanup
    MPI_Comm_rank(MPI_COMM_WORLD, (int*)&group_params.my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, (int*)&group_params.proc_count);

    group_params.procs = malloc(group_params.proc_count * sizeof(*group_params.procs));
    if (!group_params.procs) {
        fprintf(stderr, "failed to allocate procs array\n");
        goto err_worker;
    }

    group_params.distances = malloc(group_params.proc_count * sizeof(*group_params.distances));
    if (!group_params.distances) {
        fprintf(stderr, "failed to allocate distance array\n");
        free(group_params.procs);
        goto err_worker;
    }

    for (i = 0; i < group_params.proc_count; i++) {
    	group_params.procs[i]     = ucx_addresses[i];
    	group_params.distances[i] = (i == group_params.my_rank) ?
    			UCP_COLL_DISTANCE_ME : UCP_COLL_DISTANCE_FABRIC;
    }

    status = ucp_coll_group_create(ucp_worker, &group_params, &ucp_group);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to create group\n");
        goto err_worker;
    }

    coll_params.type        = UCP_GROUP_COLLECTIVE_TYPE_ALLREDUCE;
    coll_params.flags       = 0;
    coll_params.root        = 0;
    coll_params.sbuf        = &ret;
    coll_params.rbuf        = &ret;
    coll_params.datatype    = MPI_INT;
    coll_params.dcount      = 1;
    coll_params.dsize       = 4; // TODO: remove?
    coll_params.reduce.type = UCP_GROUP_COLLECTIVE_REDUCE_OP_SUM;
    coll_params.reduce.op   = get_ucx_address_by_rank;
    coll_params.cb          = NULL;

    status = ucp_coll_op_create(ucp_group, &coll_params, &ucp_coll);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to create collective\n");
        goto err_group;
    }

    ucs_status_ptr_t *request = ucp_coll_op_start(ucp_coll);
    if (UCS_PTR_IS_ERR(request)) {
    	fprintf(stderr, "failed to launch collective\n");
    	goto err_coll;
    }

    while (status == UCS_INPROGRESS) {
    	ucp_worker_progress(ucp_worker);
    	status = ucp_request_test(request, NULL);
    }

err_coll:
	ucp_coll_op_destroy(ucp_coll);

err_group:
	ucp_coll_group_destroy(ucp_group);
	free(group_params.distances);
	free(group_params.procs);

err_worker:
    ucp_worker_destroy(ucp_worker);

    ucp_cleanup(ucp_context);

err:
 	MPI_Finalize();
    return ret;
}
