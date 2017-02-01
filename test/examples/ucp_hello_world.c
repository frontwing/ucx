/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2016.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#define HAVE_CONFIG_H /* Force using config.h, so test would fail if header
                         actually tries to use it */

/*
 * UCP hello world client / server example utility
 * -----------------------------------------------
 *
 * Server side:
 *
 *    ./ucp_hello_world
 *
 * Client side:
 *
 *    ./ucp_hello_world -n <server host name>
 *
 * Notes:
 *
 *    - Client acquires Server UCX address via TCP socket
 *
 *
 * Author:
 *
 *    Ilya Nelkenbaum <ilya@nelkenbaum.com>
 *    Sergey Shalnov <sergeysh@mellanox.com> 7-June-2016
 */

#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  //getopt
#include <ctype.h>   //isprint
#include <pthread.h> //pthread_self
#include <errno.h>   //errno
#include <time.h>

#include <mpi.h>
#define S2S1 2
#define S1C 1
#define CLIENT 0
#define S1 1
#define S2 2

struct msg {
    uint64_t        data_len;
    uint8_t         data[0];
};

struct ucx_context {
    int             completed;
};

enum ucp_test_mode_t {
    TEST_MODE_PROBE,
    TEST_MODE_WAIT,
    TEST_MODE_EVENTFD
} ucp_test_mode = TEST_MODE_PROBE;

static long test_string_length = 16;
static const ucp_tag_t tag  = 0x1337a880u;
static const ucp_tag_t tag_mask = -1;
static ucp_address_t *local_addr;
static ucp_address_t *peer_addr;
static ucp_address_t *server2_addr;

static size_t local_addr_len;
static size_t peer_addr_len;
static size_t server2_addr_len;

static int parse_cmd(int argc, char * const argv[]);
static int run_server();
static int run_client(const char *server);
static void generate_random_string(char *str, int size);

static void request_init(void *request)
{
    struct ucx_context *ctx = (struct ucx_context *) request;
    ctx->completed = 0;
}

static void send_handle(void *request, ucs_status_t status)
{
    struct ucx_context *context = (struct ucx_context *) request;

    context->completed = 1;

    printf("[0x%x] send handler called with status %d\n",
           (unsigned int)pthread_self(), status);
}

static void recv_handle(void *request, ucs_status_t status,
                        ucp_tag_recv_info_t *info)
{
    struct ucx_context *context = (struct ucx_context *) request;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (length %zu)\n",
           (unsigned int)pthread_self(), status, info->length);
}

static void wait(ucp_worker_h ucp_worker, struct ucx_context *context)
{
    while (context->completed == 0) {
        ucp_worker_progress(ucp_worker);
    }
}

static ucs_status_t test_poll_wait(ucp_worker_h ucp_worker)
{
    int ret = -1;
    ucs_status_t status;
    int epoll_fd_local = 0, epoll_fd = 0;
    struct epoll_event ev;
    ev.data.u64 = 0;

    status = ucp_worker_get_efd(ucp_worker, &epoll_fd);
    if (status != UCS_OK) {
        goto err;
    }
    /* It is recommended to copy original fd */
    epoll_fd_local = epoll_create(1);

    ev.data.fd = epoll_fd;
    ev.events = EPOLLIN;
    if (epoll_ctl(epoll_fd_local, EPOLL_CTL_ADD, epoll_fd, &ev) < 0) {
        fprintf(stderr, "Couldn't add original socket %d to the "
                "new epoll: %m\n", epoll_fd);
        goto err_fd;
    }
    /* Need to prepare ucp_worker before epoll_wait */
    status = ucp_worker_arm(ucp_worker);
    if (status == UCS_ERR_BUSY) { /* some events are arrived already */
        ret = UCS_OK;
        goto err_fd;
    }
    if (status != UCS_OK) {
        goto err_fd;
    }

    do {
        ret = epoll_wait(epoll_fd_local, &ev, 1, -1);
    } while ((ret == -1) && (errno == EINTR));

    ret = UCS_OK;

err_fd:
    close(epoll_fd_local);

err:
    return ret;
}

static int run_ucx_client(ucp_worker_h ucp_worker)
{
    ucp_tag_recv_info_t info_tag;
    ucp_tag_message_h msg_tag;
    ucs_status_t status;
    ucp_ep_h server_ep;
    ucp_ep_params_t ep_params;
    struct msg *msg = 0;
    struct ucx_context *request = 0;
    size_t msg_len = 0;
    int ret = -1;

    /* Send client UCX address to server */
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address    = peer_addr;

    status = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
    if (status != UCS_OK) {
        goto err;
    }

    msg_len = sizeof(*msg) + local_addr_len;
    msg = calloc(1, msg_len);
    if (!msg) {
        goto err_ep;
    }

    msg->data_len = local_addr_len;
    memcpy(msg->data, local_addr, local_addr_len);

    request = ucp_tag_send_nb(server_ep, msg, msg_len,
                              ucp_dt_make_contig(1), tag,
                              send_handle);
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to send UCX address message\n");
        free(msg);
        goto err_ep;
    } else if (UCS_PTR_STATUS(request) != UCS_OK) {
        fprintf(stderr, "UCX address message was scheduled for send\n");
        wait(ucp_worker, request);
        request->completed = 0; /* Reset request state before recycling it */
        ucp_request_release(request);
    }

    free (msg);

    /* Receive test string from server */
    do {
        /* Following blocked methods used to polling internal file descriptor
         * to make CPU idle and don't spin loop
         */
        if (ucp_test_mode == TEST_MODE_WAIT) {
            /* Polling incoming events*/
            status = ucp_worker_wait(ucp_worker);
            if (status != UCS_OK) {
                goto err_ep;
            }
        } else if (ucp_test_mode == TEST_MODE_EVENTFD) {
            status = test_poll_wait(ucp_worker);
            if (status != UCS_OK) {
                goto err_ep;
            }
        }

        /* Progressing before probe to update the state */
        ucp_worker_progress(ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);

    msg = malloc(info_tag.length);
    if (!msg) {
        fprintf(stderr, "unable to allocate memory\n");
        goto err_ep;
    }

    request = ucp_tag_msg_recv_nb(ucp_worker, msg, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag,
                                  recv_handle);

    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to receive UCX data message (%u)\n",
                UCS_PTR_STATUS(request));
        free(msg);
        goto err_ep;
    } else {
        wait(ucp_worker, request);
        request->completed = 0;
        ucp_request_release(request);
        printf("UCX data message was received\n");
    }

    printf("\n\n----- SERVER MIGRATION TIME! ----\n\n");
    printf("%s", msg->data);
    printf("\n\n---------------------------------\n\n");

    msg_len = sizeof(*msg) + local_addr_len;
    msg = calloc(1, msg_len);
    if (!msg) {
        goto err_ep;
    }

    msg->data_len = local_addr_len;
    memcpy(msg->data, local_addr, local_addr_len);

    request = ucp_tag_send_nb(server_ep, msg, msg_len,
                              ucp_dt_make_contig(1), tag,
                              send_handle);
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to send UCX address message\n");
        free(msg);
        goto err_ep;
    } else if (UCS_PTR_STATUS(request) != UCS_OK) {
        fprintf(stderr, "UCX address message was scheduled for send\n");
        wait(ucp_worker, request);
        request->completed = 0; /* Reset request state before recycling it */
        ucp_request_release(request);
    }

    free (msg);

    /* Receive test string from server */
    do {
        /* Following blocked methods used to polling internal file descriptor
         * to make CPU idle and don't spin loop
         */
        if (ucp_test_mode == TEST_MODE_WAIT) {
            /* Polling incoming events*/
            status = ucp_worker_wait(ucp_worker);
            if (status != UCS_OK) {
                goto err_ep;
            }
        } else if (ucp_test_mode == TEST_MODE_EVENTFD) {
            status = test_poll_wait(ucp_worker);
            if (status != UCS_OK) {
                goto err_ep;
            }
        }

        /* Progressing before probe to update the state */
        ucp_worker_progress(ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);

    msg = malloc(info_tag.length);
    if (!msg) {
        fprintf(stderr, "unable to allocate memory\n");
        goto err_ep;
    }

    request = ucp_tag_msg_recv_nb(ucp_worker, msg, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag,
                                  recv_handle);

    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to receive UCX data message (%u)\n",
                UCS_PTR_STATUS(request));
        free(msg);
        goto err_ep;
    } else {
        wait(ucp_worker, request);
        request->completed = 0;
        ucp_request_release(request);
        printf("UCX data message was received\n");
    }

    printf("\n\n----- UCP TEST SUCCESS ----\n\n");
    printf("%s", msg->data);
    printf("\n\n---------------------------\n\n");

    free(msg);

    ret = 0;

err_ep:
    ucp_ep_destroy(server_ep);

err:
    return ret;
}

static int run_ucx_server(ucp_worker_h ucp_worker, int rank)
{
    ucp_tag_recv_info_t info_tag;
    ucp_tag_message_h msg_tag;
    ucs_status_t status;
    ucp_ep_h client_ep;
    ucp_ep_params_t ep_params;
    struct msg *msg = 0;
    struct ucx_context *request = 0;
    size_t msg_len = 0;
    int ret = -1;

    /* Receive client UCX address */
    do {
        /* Following blocked methods used to polling internal file descriptor
         * to make CPU idle and don't spin loop
         */
        if (ucp_test_mode == TEST_MODE_WAIT) {
            status = ucp_worker_wait(ucp_worker);
            if (status != UCS_OK) {
                goto err;
            }
        } else if (ucp_test_mode == TEST_MODE_EVENTFD) {
            status = test_poll_wait(ucp_worker);
            if (status != UCS_OK) {
                goto err;
            }
        }

        /* Progressing before probe to update the state */
        ucp_worker_progress(ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);

    msg = malloc(info_tag.length);
    if (!msg) {
        fprintf(stderr, "unable to allocate memory\n");
        goto err;
    }
    request = ucp_tag_msg_recv_nb(ucp_worker, msg, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag, recv_handle);

    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to receive UCX address message (%s)\n",
                ucs_status_string(UCS_PTR_STATUS(request)));
        free(msg);
        goto err;
    } else {
        wait(ucp_worker, request);
        request->completed = 0;
        ucp_request_release(request);
        printf("UCX address message was received\n");
    }

    peer_addr = malloc(msg->data_len);
    if (!peer_addr) {
        fprintf(stderr, "unable to allocate memory for peer address\n");
        free(msg);
        goto err;
    }

    peer_addr_len = msg->data_len;
    memcpy(peer_addr, msg->data, peer_addr_len);

    free(msg);

    /* Send test string to client */
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address    = peer_addr;

    status = ucp_ep_create(ucp_worker, &ep_params, &client_ep);
    if (status != UCS_OK) {
        goto err;
    }

    msg_len = sizeof(*msg) + test_string_length;
    msg = calloc(1, msg_len);
    if (!msg) {
        printf("unable to allocate memory\n");
        goto err_ep;
    }

    msg->data_len = msg_len - sizeof(*msg);
    generate_random_string(msg->data, test_string_length);

    request = ucp_tag_send_nb(client_ep, msg, msg_len,
                              ucp_dt_make_contig(1), tag,
                              send_handle);
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to send UCX data message\n");
        free(msg);
        goto err_ep;
    } else if (UCS_PTR_STATUS(request) != UCS_OK) {
        printf("UCX data message was scheduled for send\n");
        wait(ucp_worker, request);
        request->completed = 0;
        ucp_request_release(request);
    }

    ret = 0;
    free(msg);
	if (rank == S2) {
		ucp_ep_h other_ep;
		ep_params.address = server2_addr;
		
		status = ucp_ep_create(ucp_worker, &ep_params, &other_ep);
		if (status != UCS_OK) {
			goto err;
	    }
		
		/* cross fingers here! */
		printf("MIGRATION STARTED!");
		ucp_worker_migrate(ucp_worker, other_ep);
		printf("OMG MIGRATION COMPLETE OMG!");
		
		ucp_ep_destroy(other_ep);
	}


err_ep:
    ucp_ep_destroy(client_ep);

err:
    return ret;
}

static int run_test(int rank, ucp_worker_h ucp_worker)
{
    if (rank == CLIENT) {
        return run_ucx_client(ucp_worker);
    } else {
        return run_ucx_server(ucp_worker, rank);
    }
}

static void barrier(int oob_sock)
{
    int dummy = 0;
    send(oob_sock, &dummy, sizeof(dummy), 0);
    recv(oob_sock, &dummy, sizeof(dummy), 0);
}

static void generate_random_string(char *str, int size)
{
    int i;
    srand(time(NULL)); // randomize seed
    for (i = 0; i < (size-1); ++i) {
        str[i] =  'A' + (rand() % 26);
    }
    str[i] = 0;
}

int main(int argc, char **argv)
{
    /* UCP temporary vars */
    ucp_params_t ucp_params;
    ucp_worker_params_t worker_params;
    ucp_config_t *config;
    ucs_status_t status;

    /* UCP handler objects */
    ucp_context_h ucp_context;
    ucp_worker_h ucp_worker;

    /* OOB connection vars */
    uint64_t addr_len = 0;
    char *server = NULL;
    int oob_sock = -1;
    int ret = -1;
	int size, rank;

	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    /* Parse the command line */
    if (parse_cmd(argc, argv) != UCS_OK) {
        goto err;
    }
    /* UCP initialization */
    status = ucp_config_read(NULL, NULL, &config);
    if (status != UCS_OK) {
        goto err;
    }

    ucp_params.features = UCP_FEATURE_TAG;
    if (ucp_test_mode == TEST_MODE_WAIT || ucp_test_mode == TEST_MODE_EVENTFD) {
        ucp_params.features |= UCP_FEATURE_WAKEUP;
    }
    ucp_params.request_size    = sizeof(struct ucx_context);
    ucp_params.request_init    = request_init;
    ucp_params.request_cleanup = NULL;

    status = ucp_init(&ucp_params, config, &ucp_context);

    ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);

    ucp_config_release(config);
    if (status != UCS_OK) {
        goto err;
    }

    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
    if (status != UCS_OK) {
        goto err_cleanup;
    }

    status = ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);
    if (status != UCS_OK) {
        goto err_worker;
    }

    printf("[0x%x] local address length: %zu\n",
           (unsigned int)pthread_self(), local_addr_len);

	MPI_Status stat;
	/* Exchange all addresses */
	/* 0 is client, 1 is first server, 2 is second server */
	if(rank == CLIENT) /* client */
	{
		MPI_Recv(&peer_addr_len, 1, MPI_INT, S1, S1C, MPI_COMM_WORLD, &stat);
		peer_addr = malloc(sizeof(char)*peer_addr_len);
		MPI_Recv(peer_addr, local_addr_len, MPI_CHAR, S1, S1C, MPI_COMM_WORLD, &stat);
		/* send my address to first server. get address for second server for migration */
	}
	else if(rank == S1) /* first server */
	{
		/* Send address length first */
		MPI_Recv(&server2_addr_len, 1, MPI_INT, S2, S2S1, MPI_COMM_WORLD, &stat);
		server2_addr = malloc(sizeof(char)*server2_addr_len);
		/* send my address to client, get clients address. */
		MPI_Recv(server2_addr, server2_addr_len, MPI_CHAR, S2, S2S1, MPI_COMM_WORLD, &stat);
		MPI_Send(&local_addr_len, 1, MPI_INT, CLIENT, S1C, MPI_COMM_WORLD);
		MPI_Send(local_addr, local_addr_len, MPI_CHAR, CLIENT, S1C, MPI_COMM_WORLD);
	}

	else if(rank == S2) /* second server */
	{
		MPI_Send(&local_addr_len, 1, MPI_INT, S1, S2S1, MPI_COMM_WORLD);
		MPI_Send(local_addr, local_addr_len, MPI_CHAR, S1, S2S1, MPI_COMM_WORLD);
	}


    ret = run_test(rank, ucp_worker);

    /* Make sure remote is disconnected before destroying local worker */
	MPI_Barrier(MPI_COMM_WORLD);

err_peer_addr:
    free(peer_addr);

err_addr:
    ucp_worker_release_address(ucp_worker, local_addr);

err_worker:
    ucp_worker_destroy(ucp_worker);

err_cleanup:
    ucp_cleanup(ucp_context);

err:
	MPI_Finalize();
    return ret;
}

int parse_cmd(int argc, char * const argv[])
{
    int c = 0, index = 0;
    opterr = 0;
    while ((c = getopt(argc, argv, "wfbs:h")) != -1) {
        switch (c) {
        case 'w':
            ucp_test_mode = TEST_MODE_WAIT;
            break;
        case 'f':
            ucp_test_mode = TEST_MODE_EVENTFD;
            break;
        case 'b':
            ucp_test_mode = TEST_MODE_PROBE;
            break;
        case 's':
            test_string_length = atol(optarg);
            if (test_string_length <= 0) {
                fprintf(stderr, "Wrong string size %ld\n", test_string_length);
                return UCS_ERR_UNSUPPORTED;
            }	
            break;
        case '?':
            if (optopt == 's') {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            } else if (isprint (optopt)) {
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            } else {
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            }
        case 'h':
        default:
            fprintf(stderr, "Usage: ucp_hello_world [parameters]\n");
            fprintf(stderr, "UCP hello world client/server example utility\n");
            fprintf(stderr, "\nParameters are:\n");
            fprintf(stderr, "  -w      Select test mode \"wait\" to test "
                    "ucp_worker_wait function\n");
            fprintf(stderr, "  -f      Select test mode \"event fd\" to test "
                    "ucp_worker_get_efd function with later poll\n");
            fprintf(stderr, "  -b      Select test mode \"busy polling\" to test "
                    "ucp_tag_probe_nb and ucp_worker_progress (default)\n");
            fprintf(stderr, "  -n name Set node name or IP address "
                    "of the server (required for client and should be ignored "
                    "for server)\n");
            fprintf(stderr, "  -p port Set alternative server port (default:13337)\n");
            fprintf(stderr, "  -s size Set test string length (default:16)\n");
            fprintf(stderr, "\n");
            return UCS_ERR_UNSUPPORTED;
        }
    }
    fprintf(stderr, "INFO: UCP_HELLO_WORLD mode = %d\n",
            ucp_test_mode);

    for (index = optind; index < argc; index++) {
        fprintf(stderr, "WARNING: Non-option argument %s\n", argv[index]);
    }
    return UCS_OK;
}


