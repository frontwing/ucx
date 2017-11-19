/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2017.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <ucs/debug/tune.h>

extern ucs_tune_ctx_t ucs_tune_ctx;

int main(int argc, char **argv)
{
    int prof_fds[2];
    struct stat temp;
    char *tuning_path = NULL, *param_name, *param_value;
    ucs_status_t error = UCS_ERR_IO_ERROR;

    if ((argc < 2) || (argc > 4) || (argv[1][0] == '-')) {
        printf("Usage: ucs_params [<param_name> [[<param_new_value>] <tuning_path>]]\n");
        return -1;
    }

    param_name = argv[1];
    if (argc == 4) {
        param_value = argv[2];
        tuning_path = argv[3];
    } else {
        param_value = NULL;
        if (argc == 3) {
            tuning_path = argv[2];
        } else {
            tuning_path = ucs_tune_ctx.tune_cmd_path;
        }
    }

    if (0 > stat(tuning_path, &temp)) {
        perror(tuning_path);
        goto cleanup;
    }

    error = ucs_tune_open_cmd(prof_fds, tuning_path);
    if (error != UCS_OK) {
        perror(ucs_status_string(error));
        goto cleanup;
    }

    if (param_name) {
        if (param_value) {
            (void) ucs_tune_send_cmd_set_param(prof_fds, param_name, param_value);
        } else {
            error = ucs_tune_send_cmd_get_param(prof_fds, param_name, &param_value);
            if (error == UCS_OK) {
                printf("%s=%s\n", param_name, param_value);
            }
            free(param_value);
        }
    } else {
        error = ucs_tune_send_cmd_enum_ctxs(prof_fds, &param_value);
        if (error == UCS_OK) {
            printf("%s\n", param_value);
        }
        free(param_value);
    }

    close(prof_fds[1]);
    close(prof_fds[0]);
cleanup:
    return (error == UCS_OK) ? 0 : -1;
}

