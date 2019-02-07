/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#include "ucg_plan.h"

#include <ucg/api/ucg.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack.h>
#include <ucs/type/class.h>
#include <ucs/sys/module.h>
#include <ucs/sys/string.h>
#include <ucs/arch/cpu.h>

UCS_LIST_HEAD(ucg_plan_components_list);

ucs_config_field_t ucg_plan_config_table[] = {
  {NULL}
};

/**
 * Keeps information about allocated configuration structure, to be used when
 * releasing the options.
 */
typedef struct ucg_config_bundle {
    ucs_config_field_t *table;
    const char         *table_prefix;
    char               data[];
} ucg_config_bundle_t;


ucs_status_t ucg_plan_query(ucg_plan_desc_t **resources_p,
                                      unsigned *nums_p)
{
    UCS_MODULE_FRAMEWORK_DECLARE(ucg);
    ucg_plan_desc_t *resources, *topos, *tmp;
    unsigned i, nums, num_topos;
    ucg_plan_component_t *topoc;
    ucs_status_t status;

    UCS_MODULE_FRAMEWORK_LOAD(ucg);

    resources     = NULL;
    nums = 0;

    ucs_list_for_each(topoc, &ucg_plan_components_list, list) {
        status = topoc->query(&topos, &num_topos);
        if (status != UCS_OK) {
            ucs_debug("Failed to query %s* resources: %s", topoc->name,
                      ucs_status_string(status));
            continue;
        }

        if (num_topos == 0) {
            ucs_free(topos);
            continue;
        }

        tmp = ucs_realloc(resources,
                          sizeof(*resources) * (nums + num_topos),
                          "topos");
        if (tmp == NULL) {
            ucs_free(topos);
            status = UCS_ERR_NO_MEMORY;
            goto err;
        }

        for (i = 0; i < num_topos; ++i) {
            ucs_assertv_always(!strncmp(topoc->name, topos[i].plan_name,
                                       strlen(topoc->name)),
                               "Topology name must begin with topology component name."
                               "Topology name: %s MD component name: %s ",
                               topos[i].plan_name, topoc->name);
        }
        resources = tmp;
        memcpy(resources + nums, topos,
               sizeof(*topos) * num_topos);
        nums += num_topos;
        ucs_free(topos);
    }

    *resources_p     = resources;
    *nums_p = nums;
    return UCS_OK;

err:
    ucs_free(resources);
    return status;
}

void ucg_plan_release_list(ucg_plan_desc_t *resources)
{
    ucs_free(resources);
}

ucs_status_t ucg_plan_single(ucg_plan_component_t *topoc,
                                      ucg_plan_desc_t **resources_p,
                                      unsigned *nums_p)
{
    ucg_plan_desc_t *resource;

    resource = ucs_malloc(sizeof(*resource), "topo resource");
    if (resource == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    ucs_snprintf_zero(resource->plan_name, UCG_PLAN_COMPONENT_NAME_MAX, "%s", topoc->name);

    *resources_p     = resource;
    *nums_p = 1;
    return UCS_OK;
}

ucs_status_t ucg_plan_select_component(ucg_plan_desc_t *planners,
                                       unsigned *num_planners,
                                       const ucg_group_params_t *group_params,
                                       const ucg_collective_params_t *coll_params,
                                       ucg_plan_component_t **planc)
{
    *planc = planners[0].plan_component;
    return UCS_OK;
}
