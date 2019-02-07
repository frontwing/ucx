/*
* Copyright (C) Huawei Technologies Co., Ltd. 2018.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifndef UCG_PLAN_COMPONENT_H_
#define UCG_PLAN_COMPONENT_H_

#include <string.h>

#include <ucg/api/ucg.h>
#include <ucs/config/parser.h>
#include <ucs/datastruct/list_types.h>

BEGIN_C_DECLS

typedef uint32_t ucg_coll_id_t;
typedef struct ucg_plan_component ucg_plan_component_t;

/**
 * @ingroup UCG_RESOURCE
 * @brief Collective planning resource descriptor.
 *
 * This structure describes a collective operation planning resource.
 */
typedef struct ucg_plan_desc {
    char                    plan_name[UCG_PLAN_COMPONENT_NAME_MAX]; /**< Plan name */
    ucg_plan_component_t   *plan_component;
    char                   *supported_collectives; // TODO: define type!
    struct {
        double              launch_nb_single_short_send;
        double              launch_nbr_single_short_send;
    } latency;

} ucg_plan_desc_t;

/**
 * "Base" structure which defines planning configuration options.
 * Specific planning components extend this structure.
 */
struct ucg_plan_config {
    /* C standard prohibits empty structures */
    char                    __dummy;
};

typedef struct ucg_request {
    ucs_status_t            status;  /* Operation status */
    uint16_t                flags;   /* Request flags */
    char                    priv[0];
} ucg_request_t;

typedef struct ucg_plan {
    ucs_list_link_t         list;
    ucg_collective_params_t params;
    ucg_plan_component_t   *planner;
    ucg_worker_h            worker;
    char                    priv[0];
} ucg_plan_t;

struct ucg_plan_component {
    /* test for support and other attribures of this component */
    ucs_status_t          (*query)(ucg_plan_desc_t **resources_p, unsigned *nums_p);
    /* plan a collective operation with this component */
    ucs_status_t          (*plan)(ucg_plan_component_t *plan_component,
                                  const ucg_group_params_t *group_params,
                                  const ucg_collective_params_t *coll_params,
                                  ucg_plan_t **plan_p);
    /* start a new collective, create a request object */
    ucs_status_t          (*launch_nb)(ucg_plan_t *plan, ucg_coll_id_t coll_id, ucg_request_t **req);
    /* start a new collective, by using a recycled request (originally from @ref launch_nb )*/
    ucs_status_t          (*launch_nbr)(ucg_plan_t *plan, ucg_coll_id_t coll_id, ucg_request_t *req);
    /* destroy a plan object */
    void                  (*destroy)(ucg_plan_t *plan);

    const char              name[UCG_PLAN_COMPONENT_NAME_MAX];
    void                   *priv;
    const char             *cfg_prefix;        /**< Prefix for configuration environment vars */
    ucs_config_field_t     *plan_config_table; /**< Defines MD configuration options */
    size_t                  plan_config_size;  /**< MD configuration structure size */
    ucs_list_link_t         list;
};

/**
 * Define a planning component.
 *
 * @param _planc         Planning component structure to initialize.
 * @param _name          Planning component name.
 * @param _query         Function to query planning resources.
 * @param _plan_ucp      Function to plan a collective from UCP requests.
 * @param _plan_uct      Function to plan a collective with UCT objects.
 * @param _priv          Custom private data.
 * @param _cfg_prefix    Prefix for configuration environment variables.
 * @param _cfg_table     Defines the planning component's configuration values.
 * @param _cfg_struct    Planning component configuration structure.
 */
#define UCG_PLAN_COMPONENT_DEFINE(_planc, _name, _query, _plan, _launch_nb, \
                                  _launch_nbr, _destroy, _priv, \
                                  _cfg_prefix, _cfg_table, _cfg_struct) \
    \
    ucg_plan_component_t _planc = { \
        .query             = _query, \
        .plan              = _plan, \
        .launch_nb         = _launch_nb, \
        .launch_nbr        = _launch_nbr, \
        .destroy           = _destroy, \
        .cfg_prefix        = _cfg_prefix, \
        .plan_config_table = _cfg_table, \
        .plan_config_size  = sizeof(_cfg_struct), \
        .priv              = _priv, \
        .name              = _name, \
    }; \
    UCS_STATIC_INIT { \
        ucs_list_add_tail(&ucg_plan_components_list, &_planc.list); \
    } \
    UCS_CONFIG_REGISTER_TABLE(_cfg_table, _name" topology", _cfg_prefix, \
                              _cfg_struct)


static UCS_F_ALWAYS_INLINE void*
ucg_plan_fill_name(ucg_plan_component_t *plan, void *buffer)
{
    memcpy(buffer, plan->name, UCG_PLAN_COMPONENT_NAME_MAX);
    return (char*)buffer + UCG_PLAN_COMPONENT_NAME_MAX;
}

ucs_status_t ucg_plan_single(ucg_plan_component_t *planc,
                             ucg_plan_desc_t **resources_p,
                             unsigned *nums_p);

ucs_status_t ucg_plan_connect(ucg_group_h group,
                              ucg_group_member_index_t idx,
                              ucp_ep_h *ep_p);

extern ucs_list_link_t ucg_plan_components_list;
extern ucs_config_field_t ucg_plan_config_table[];

END_C_DECLS

#endif
