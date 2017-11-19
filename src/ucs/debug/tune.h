/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2017.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef UCX_TUNE_H_
#define UCX_TUNE_H_

#include <pthread.h>
#include <ucs/type/status.h>
#include <ucs/datastruct/list_types.h>

typedef enum ucs_tune_cmd {
    UCS_TUNE_CMD_ENUM_CTXS,
    UCS_TUNE_CMD_GET_PARAM,
    UCS_TUNE_CMD_SET_PARAM,
    UCS_TUNE_CMD_TERMINATE
} ucs_tune_cmd_t;

typedef struct ucs_tune_ctx {
    char* tune_cmd_path;    /* Path to input file, NULL when off */
    pthread_t tune_thread;  /* Thread handling the tuning inside UCX */
    int tune_server_fds[2]; /* Server FDs, created on init */
} ucs_tune_ctx_t;

/* Global context for tuning UCX */
extern ucs_tune_ctx_t ucs_tune_ctx;

/* Global list of contexts, defined in mxm.c */
extern pthread_mutex_t ucs_context_list_lock;
extern ucs_list_link_t ucs_context_list;

/* Init/cleanup (for UCP) */
void ucs_tune_init();
void ucs_tune_cleanup();

/* This is the external API */
ucs_status_t ucs_tune_open_cmd(int tune_fd[2], const char* tuning_path);
ucs_status_t ucs_tune_send_cmd_set_param(int tune_fd[2], const char* param_name, const char* param_value);
ucs_status_t ucs_tune_send_cmd_get_param(int tune_fd[2], const char* param_name, char** param_value);
ucs_status_t ucs_tune_send_cmd_enum_ctxs(int tune_fd[2], char** ctxs_list_output);
ucs_status_t ucs_tune_send_cmd_close(int tune_fd[2]);

#endif // UCX_TUNE_H_
