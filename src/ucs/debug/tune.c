/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2017.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#define _GNU_SOURCE
#include "tune.h"

#include <ucp/core/ucp_context.h>
#include <ucp/core/ucp_worker.h>
#include <ucp/core/ucp_ep.h>

#include <ucs/config/parser.h>
#include <ucs/sys/string.h>

#define UCS_TUNE_OUTPUT_SUFFIX "_out"
#define UCX_VAR_PREFIX_LEN (4) // prefix is "UCX_"

#define UCS_TUNE_NAME_EMPTY ""
#define UCS_TUNE_NAME_DELIM ":"
#define UCS_TUNE_PARAM_MAX_LEN (1000)

#define UCS_TUNE_CLOSE(fds) { close(fds[0]); close(fds[1]); }

/* Global profiling context */
ucs_tune_ctx_t ucs_tune_ctx = {0};

pthread_mutex_t ucs_context_list_lock = PTHREAD_MUTEX_INITIALIZER;
UCS_LIST_HEAD(ucs_context_list);

extern ucs_config_field_t ucs_global_opts_table[];
extern ucs_config_field_t ucp_config_table[];

typedef struct param_ptr {
    ucp_context_t *context;
    ucp_worker_t *worker;
    ucp_ep_t *endpoint;
    void* ptr;
} param_ptr_t;

ucs_status_t ucs_tune_write_buf(int fd, const void* buf, size_t len)
{
    size_t total = 0;
    int ret = 0;

    do {
        total += ret;
        ret = write(fd, buf + total, len - total);
    } while (ret > 0);

    if (total != len) {
        return UCS_ERR_IO_ERROR;
    }
    return UCS_OK;
}


static ucs_status_t ucs_tune_read_buf(int fd, void* buf, size_t len)
{
    size_t total = 0;
    int ret = 0;

    do {
        ret = read(fd, buf + total, len - total);
        total += ret;
    } while ((ret > 0) && (total < len));

    if (ret == 0) {
        return UCS_ERR_NO_MESSAGE;
    }

    if (ret < 0) {
        return UCS_ERR_IO_ERROR;
    }
    return UCS_OK;
}

ucs_status_t ucs_tune_write_string(int tune_fd[2], const char* str)
{
    size_t sent = strlen(str) + 1;

    ucs_status_t error = ucs_tune_write_buf(tune_fd[0], &sent, sizeof(sent));
    if (error != UCS_OK) {
        return error;
    }

    return ucs_tune_write_buf(tune_fd[0], str, sent);
}

ucs_status_t ucs_tune_read_string(int fd, char **out_str)
{
    int ret = 0;
    ucs_status_t error;
    size_t expected = 0, total = 0;

    error = ucs_tune_read_buf(fd, &expected, sizeof(expected));
    if (error != UCS_OK) {
        return error;
    }

    *out_str = ucs_malloc(expected, "tune strings");
    if (*out_str == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    do {
        ret = read(fd, *out_str + total, expected - total);
        total += ret;
    } while ((ret > 0) && (total < expected));

    if (ret > 0) {
        return UCS_OK;
    }

    ucs_free(*out_str);
    return UCS_ERR_IO_ERROR;
}

inline ucs_status_t ucs_tune_write_cmd(int tune_fd[2], ucs_tune_cmd_t cmd)
{
    return ucs_tune_write_buf(tune_fd[0], &cmd, sizeof(cmd));
}

ucs_status_t ucs_tune_open_cmd_internal(int tune_fd[2], const char* tuning_path,
                                        int is_server, int* status_fd)
{
    int fd;
    struct stat temp;
    ucs_status_t error = UCS_OK;
    char filename[FILENAME_MAX];

    if (ucs_tune_ctx.tune_cmd_path == NULL) {
        ucs_fill_filename_template(tuning_path, filename, sizeof(filename));
        ucs_trace("tune input file path: %s\n", filename);
        ucs_tune_ctx.tune_cmd_path = strdup(filename);
    } else {
        strncpy(filename, ucs_tune_ctx.tune_cmd_path, FILENAME_MAX);
    }

    if (is_server) {
        /* Create input pipe from configured path */
        if ((0 > stat(filename, &temp)) &&
            (0 > mkfifo(filename, S_IRWXU | S_IRWXG | S_IRWXO))) {
            ucs_error("Failed to create tuning command pipe at %s: %m", filename);
            error = UCS_ERR_UNSUPPORTED;
            goto server_error;
        }

        /* Add a suffix to create the output pipe */
        sprintf(filename + strnlen(filename,
                FILENAME_MAX - strlen(UCS_TUNE_OUTPUT_SUFFIX)),
                UCS_TUNE_OUTPUT_SUFFIX);

        if ((0 > stat(filename, &temp)) &&
            (0 > mkfifo(filename, S_IRWXU | S_IRWXG | S_IRWXO))) {
            ucs_error("Failed to create tuning output pipe at %s: %m", filename);
            error = UCS_ERR_UNSUPPORTED;
        }

server_error:
        if (NULL != status_fd) {
            int fd = *status_fd;
            (void) ucs_tune_write_buf(fd, &error, sizeof(error));
            close(fd);
        }

        if (error != UCS_OK) {
            goto open_cleanup;
        }
    } else {
        sprintf(filename + strnlen(filename,
                FILENAME_MAX - strlen(UCS_TUNE_OUTPUT_SUFFIX)),
                UCS_TUNE_OUTPUT_SUFFIX);
    }

    /* Open both pipes - will block until somebody connects */
    if ((fd = open(ucs_tune_ctx.tune_cmd_path, is_server ? O_RDONLY : O_WRONLY)) < 0) {
        ucs_error("Failed to open tuning command pipe at %s: %m", ucs_tune_ctx.tune_cmd_path);
        error = UCS_ERR_UNREACHABLE;
        goto open_cleanup;
    }
    tune_fd[is_server] = fd;

    if ((fd = open(filename, is_server ? O_WRONLY : O_RDONLY)) < 0) {
        close(tune_fd[is_server]);
        if (is_server) {
            unlink(ucs_tune_ctx.tune_cmd_path);
        }
        ucs_error("Failed to open tuning output pipe at %s: %m", filename);
        error = UCS_ERR_UNREACHABLE;
        goto open_cleanup;
    }
    tune_fd[1-is_server] = fd;

    return UCS_OK;

open_cleanup:
    if ((error != UCS_OK) && (ucs_tune_ctx.tune_cmd_path != NULL)) {

        free(ucs_tune_ctx.tune_cmd_path);
        ucs_tune_ctx.tune_cmd_path = NULL;
    }
    return error;
}

static ucs_status_t ucs_tune_gen_ctxs_list(char **final_output)
{
    int ret = 0;
    ucp_ep_t *ep_iter;
    ucp_context_t *ctx_iter;
    ucp_worker_t *worker_iter;
    ucs_status_t error = UCS_OK;
    uint64_t dest_uuid UCS_V_UNUSED;

    char *output = strdup(UCS_TUNE_NAME_EMPTY);

    pthread_mutex_lock(&ucs_context_list_lock);

    ucs_list_for_each(ctx_iter, &ucs_context_list, tune_contexts) {
        ucs_list_for_each(worker_iter, &ctx_iter->tune_workers, tune_list) {
            char *tmp = output;
            ret = asprintf(&output, "%s%s\n", output, worker_iter->name);
            free(tmp);
            if (ret < 0) {
                break;
            }

            kh_foreach(&worker_iter->ep_hash, dest_uuid, ep_iter, {
                char *tmp = output;
                ret = asprintf(&output, "%s %s\n", output, ep_iter->peer_name);
                free(tmp);
                if (ret < 0) {
                    break;
                }
            })
        }
    }

    if (ret < 0) {
        error = UCS_ERR_NO_MEMORY;
        free(output);
    } else {
        *final_output = output;
    }
    pthread_mutex_unlock(&ucs_context_list_lock);
    return error;
}

enum ucs_tune_param_type {
    UCS_TUNE_PARAM_UCP_CONTEXT = 0,
    UCS_TUNE_PARAM_UCP_ENDPOINT,
    UCS_TUNE_PARAM_LAST
};

static ucs_status_t ucs_tune_param(void *opts, ucs_config_field_t *fields,
                                   char* name, char* value, size_t max)
{
    if (max) {
        return ucs_config_parser_get_value(opts, fields, name, value, max);
    }
    return  ucs_config_parser_set_value(opts, fields, name, value);
}

static ucs_status_t ucs_tune_param_by_name(char* name, char* value, size_t max)
{
    ucs_status_t status = UCS_ERR_NO_ELEM;
    enum ucs_tune_param_type param_type;
    uint64_t dest_uuid UCS_V_UNUSED;
    ucp_worker_t *worker_iter;
    ucp_context_t *ctx_iter;
    ucp_ep_t *ep_iter;
    char *worker_name;
    char *var_name;
    char *ep_name;

    /* Parse the ctx/worker/endpoint out of the variable name */
    worker_name = strtok(name, UCS_TUNE_NAME_DELIM);
    if (worker_name == NULL) {
        return UCS_ERR_INVALID_PARAM;
    }

    ep_name = strtok(NULL, UCS_TUNE_NAME_DELIM);
    if (ep_name == NULL) {
        var_name = worker_name;
        worker_name = NULL;
    } else {
        var_name = strtok(NULL, UCS_TUNE_NAME_DELIM);
        if (var_name == NULL) {
            var_name = ep_name;
            ep_name = NULL;
        } else if (NULL != strtok(NULL, UCS_TUNE_NAME_DELIM)) {
                return UCS_ERR_INVALID_PARAM;
        }
    }

    /* Look for this parameter in all known config descriptions */
    pthread_mutex_lock(&ucs_context_list_lock);
    for (param_type = 0;
         (param_type < UCS_TUNE_PARAM_LAST) && (status == UCS_ERR_NO_ELEM);
         param_type++)
    {
        /* Choose the right parameter description */
        switch (param_type) {
        case UCS_TUNE_PARAM_UCP_CONTEXT:
            ucs_list_for_each(ctx_iter, &ucs_context_list, tune_contexts) {
                if (status == UCS_ERR_NO_ELEM) {
                    /* Access to params according to the lookup table*/
                    status = ucs_tune_param(&ctx_iter->config.ext,
                            ucs_global_opts_table, var_name, value, max);
                }
            }
            break;

        case UCS_TUNE_PARAM_UCP_ENDPOINT:
            ucs_list_for_each(ctx_iter, &ucs_context_list, tune_contexts) {
                ucs_list_for_each(worker_iter, &ctx_iter->tune_workers, tune_list) {
                    if (!worker_name || !strcmp(worker_name, worker_iter->name)) {
                        kh_foreach(&worker_iter->ep_hash, dest_uuid, ep_iter, {
                            if (!ep_name || !strcmp(ep_name, ep_iter->peer_name)) {
                                ucp_ep_config_t *opts =
                                        &ep_iter->worker->ep_config[ep_iter->cfg_index];
                                if (status == UCS_ERR_NO_ELEM) {
                                    /* Access to params according to the lookup table*/
                                    status = ucs_tune_param(opts, ucp_config_table,
                                            var_name, value, max);
                                    if ((status == UCS_OK) && (max == 0)) {
                                        /* user called SET - apply changes... */
                                        ucp_ep_config_init(worker_iter, opts);
                                    }
                                }
                            }
                        });
                    }
                }
            }
            break;

        default:
            status = UCS_ERR_INVALID_PARAM;
        }
    }
    pthread_mutex_unlock(&ucs_context_list_lock);
    return status;
}

static void* ucs_tune_thread_func(void *pipe_write_fd)
{
    int tune_fd[2] = {0};
    ucs_status_t error;
    ucs_tune_cmd_t cmd;
    char *param_name = NULL;
    char *param_value = NULL;
    char param_output[UCS_TUNE_PARAM_MAX_LEN];

    do {
        /* (re-)open command channel */
        error = ucs_tune_open_cmd_internal(tune_fd, ucs_global_opts.tuning_path,
                                           1, (int*)pipe_write_fd);
        pipe_write_fd = NULL; // only report back the first time (closed in function)

        if (error != UCS_OK) {
            return NULL;
        }

        do {
            /* Read the next command */
            error = ucs_tune_read_buf(tune_fd[1], &cmd, sizeof(cmd));
            if (error != UCS_OK) {
                cmd = UCS_TUNE_CMD_TERMINATE;
            }

            switch (cmd) {
            case UCS_TUNE_CMD_ENUM_CTXS:
                error = ucs_tune_gen_ctxs_list(&param_value);
                if (error != UCS_OK) {
                    break;
                }

                (void) ucs_tune_write_string(tune_fd, param_value);
                ucs_free(param_value);
                break;

            case UCS_TUNE_CMD_GET_PARAM:
                error = ucs_tune_read_string(tune_fd[1], &param_name);
                if (error != UCS_OK) {
                    break;
                }

                error = ucs_tune_param_by_name(param_name, param_output,
                                               UCS_TUNE_PARAM_MAX_LEN);
                if (error != UCS_OK) {
                    (void) ucs_tune_write_string(tune_fd, UCS_TUNE_NAME_EMPTY);
                    break;
                }

                (void) ucs_tune_write_string(tune_fd, param_output);
                ucs_free(param_name);
                break;

            case UCS_TUNE_CMD_SET_PARAM:
                error = ucs_tune_read_string(tune_fd[1], &param_name);
                if (error != UCS_OK) {
                    break;
                }

                error = ucs_tune_read_string(tune_fd[1], &param_value);
                if (error != UCS_OK) {
                    break;
                }

                error = ucs_tune_param_by_name(param_name, param_value, 0);
                if (error != UCS_OK) {
                    (void) ucs_tune_write_string(tune_fd, UCS_TUNE_NAME_EMPTY);
                    break;
                }

                ucs_free(param_name);
                ucs_free(param_value);
                break;

            case UCS_TUNE_CMD_TERMINATE:
            default:
                break;
            }
        } while (cmd != UCS_TUNE_CMD_TERMINATE);

        UCS_TUNE_CLOSE(tune_fd);
    } while (error == UCS_ERR_NO_MESSAGE);

    return NULL;
}

void ucs_tune_init()
{
    ucs_status_t error;
    int ret, pipe_fd[2] = {0};
    ret = pipe(pipe_fd);
    if (ret != 0) {
        ucs_error("pipe() returned %d: %m", ret);
        return;
    }

    ret = pthread_create(&ucs_tune_ctx.tune_thread, NULL, ucs_tune_thread_func, (void*)&pipe_fd[1]);
    if (ret != 0) {
        ucs_error("pthread_create() returned %d: %m", ret);
        UCS_TUNE_CLOSE(pipe_fd);
        return;
    }

    if ((UCS_OK != ucs_tune_read_buf(pipe_fd[0], &error, sizeof(error))) ||
            (error != UCS_OK)) {
        ucs_error("ucs_tune_thread_func() failed and sent an error: %s", ucs_status_string(error));
        UCS_TUNE_CLOSE(pipe_fd);
        return;
    }

    close(pipe_fd[0]); /* other pipe FD is closed by the tune thread */
}

void ucs_tune_cleanup()
{
    char *path = NULL;

    if (ucs_tune_ctx.tune_cmd_path != NULL) {
        pthread_cancel(ucs_tune_ctx.tune_thread);
        pthread_join(ucs_tune_ctx.tune_thread, (void**)path);

        unlink(ucs_tune_ctx.tune_cmd_path);
        if (0 < asprintf(&path, "%s%s", ucs_tune_ctx.tune_cmd_path, UCS_TUNE_OUTPUT_SUFFIX)) {
            unlink(path);
            free(path);
        }

        free(ucs_tune_ctx.tune_cmd_path);
    }
}

ucs_status_t ucs_tune_open_cmd(int tune_fd[2], const char* tuning_path)
{
    return ucs_tune_open_cmd_internal(tune_fd, tuning_path, 0, NULL);
}

ucs_status_t ucs_tune_send_cmd_set_param(int tune_fd[2], const char* param_name, const char* param_value)
{
    ucs_status_t error = ucs_tune_write_cmd(tune_fd, UCS_TUNE_CMD_SET_PARAM);
    if (error != UCS_OK) {
        return error;
    }

    error = ucs_tune_write_string(tune_fd, param_name);
    if (error != UCS_OK) {
        return error;
    }

    return ucs_tune_write_string(tune_fd, param_value);
}

ucs_status_t ucs_tune_send_cmd_get_param(int tune_fd[2], const char* param_name, char** param_value)
{
    ucs_status_t error = ucs_tune_write_cmd(tune_fd, UCS_TUNE_CMD_GET_PARAM);
    if (error != UCS_OK) {
        return error;
    }

    error = ucs_tune_write_string(tune_fd, param_name);
    if (error != UCS_OK) {
        return error;
    }

    error =  ucs_tune_read_string(tune_fd[1], param_value);
    return error;
}

ucs_status_t ucs_tune_send_cmd_enum_ctxs(int tune_fd[2], char** ctxs_list_output)
{
    ucs_status_t error = ucs_tune_write_cmd(tune_fd, UCS_TUNE_CMD_ENUM_CTXS);
    if (error != UCS_OK) {
        return error;
    }

    return ucs_tune_read_string(tune_fd[1], ctxs_list_output);
}

ucs_status_t ucs_tune_send_cmd_close(int tune_fd[2])
{
    return ucs_tune_write_cmd(tune_fd, UCS_TUNE_CMD_TERMINATE);
}
