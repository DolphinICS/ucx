#include <uct/api/uct.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define ERROR_CHECK_UCS_OK(func_name, error) \
if (error != UCS_OK) {  \
    fprintf(stderr, "Error %s, line %u: %s failed, error code %d", __FUNCTION__, __LINE__, func_name, (int)error); \
    exit(EXIT_FAILURE); \
}

typedef enum {
    FUNC_AM_SHORT,
    FUNC_AM_BCOPY,
    FUNC_AM_ZCOPY
} func_am_t;

static ucs_status_t dev_tl_lookup(uct_md_h *md, uct_md_attr_t *md_attr, uct_tl_resource_desc_t **tl_res)
{
    uct_component_h *components;
    unsigned num_components;
    ucs_status_t status;

    status = uct_query_components(&components, &num_components);
    ERROR_CHECK_UCS_OK("uct_query_components", status)

    printf("num_components = %u\n", num_components);

    int tcp_index = -1;

    uct_component_attr_t component_attr;
    for (int cmpt_index = 0; cmpt_index < num_components; cmpt_index++) {

        printf("cmpt_index == %d\n", cmpt_index);


        component_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCE_COUNT;
        status = uct_component_query(components[cmpt_index], &component_attr);
        ERROR_CHECK_UCS_OK("uct_query_components", status)
        printf("number of resources %u\n", component_attr.md_resource_count);

        component_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCES;
        component_attr.md_resources = alloca(sizeof(*component_attr.md_resources) *
                                             component_attr.md_resource_count);
        status = uct_component_query(components[cmpt_index], &component_attr);
        ERROR_CHECK_UCS_OK("uct_query_components", status)
        printf("md_resource_count = %u\n", component_attr.md_resource_count);

        component_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_NAME;
        component_attr.md_resources = alloca(sizeof(*component_attr.md_resources) *
                                             component_attr.md_resource_count);
        status = uct_component_query(components[cmpt_index], &component_attr);
        ERROR_CHECK_UCS_OK("uct_query_components", status)
        printf("component attribute name = %s\n", component_attr.name);
        
        /* Put transport you're looking for here, search can technically end here in a real program */
        if (strncmp(component_attr.name, "tcp", 3) == 0) {
            tcp_index = cmpt_index;
        }
    }

    printf("tcp_index == %d\n", tcp_index);

    int eno1_index = -1;

    uct_tl_resource_desc_t *tl_resources = NULL;
    /* Iterate through memory domain resources */
    for (int md_index = 0; md_index < component_attr.md_resource_count; ++md_index) {

        printf("md_index == %d\n", md_index);

        uct_md_config_t *md_config;
        status = uct_md_config_read(components[tcp_index], NULL, NULL,
                                        &md_config);
        ERROR_CHECK_UCS_OK("uct_md_config_read", status)
        status = uct_md_open(components[tcp_index],
                                 component_attr.md_resources[md_index].md_name,
                                 md_config, md);
        uct_config_release(md_config);
        ERROR_CHECK_UCS_OK("uct_md_open", status)

        status = uct_md_query(*md, md_attr);
        ERROR_CHECK_UCS_OK("uct_md_query", status)

        unsigned int num_tl_resources;
        // Apparently, this should be freed with uct_release_tl_resource_list
        status = uct_md_query_tl_resources(*md, &tl_resources,
                                               &num_tl_resources);
        ERROR_CHECK_UCS_OK("uct_md_query_tl_resources", status)

        /* Put the device you're looking for here, search can technically end here in a real program.
         * tl_name will always correspond to the transport we searched for earlier, so checking it is redundant */
        for (int tl_index = 0; tl_index < num_tl_resources; ++tl_index) {
            printf("tl_index == %d\n", tl_index);
            printf("tl_name = %s , dev_name = %s\n", tl_resources[tl_index].tl_name, tl_resources[tl_index].dev_name);

            if (strncmp(tl_resources[tl_index].dev_name, "eno1", 4) == 0) {
                eno1_index = tl_index;
            }
        }
    }

    printf("eno1_index == %d\n", eno1_index);

    if (tl_resources != NULL) {
        *tl_res = &tl_resources[eno1_index];
    } else {
        return UCS_ERR_NO_RESOURCE;
    }

    return UCS_OK;
}

static ucs_status_t init_iface(
    char *dev_name,
    char *tl_name,
    uct_iface_attr_t *iface_attr,
    uct_iface_h *iface,
    uct_md_h md,
    uct_worker_h worker)
{
    ucs_status_t status;
    uct_iface_config_t *config; /* Defines interface configuration options */
    uct_iface_params_t params;

    params.field_mask = UCT_IFACE_PARAM_FIELD_OPEN_MODE   |
                        UCT_IFACE_PARAM_FIELD_DEVICE      |
                        UCT_IFACE_PARAM_FIELD_STATS_ROOT  |
                        UCT_IFACE_PARAM_FIELD_RX_HEADROOM |
                        UCT_IFACE_PARAM_FIELD_CPU_MASK;
    params.open_mode            = UCT_IFACE_OPEN_MODE_DEVICE;
    params.mode.device.tl_name  = tl_name;
    params.mode.device.dev_name = dev_name;
    params.stats_root           = NULL;
    params.rx_headroom          = sizeof(int);

    UCS_CPU_ZERO(&params.cpu_mask);
    /* Read transport-specific interface configuration */
    status = uct_md_iface_config_read(md, tl_name, NULL, NULL, &config);
    ERROR_CHECK_UCS_OK("uct_md_iface_config_read", status)

    /* Open communication interface */
    status = uct_iface_open(md, worker, &params, config,
                            iface);
    uct_config_release(config);
    ERROR_CHECK_UCS_OK("uct_md_iface_config_read", status)

    /* Enable progress on the interface */
    uct_iface_progress_enable(*iface,
                              UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    /* Get interface attributes */
    status = uct_iface_query(*iface, iface_attr);
    ERROR_CHECK_UCS_OK("uct_md_iface_config_read", status)

    if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_SHORT) {
        printf("UCT_IFACE_FLAG_AM_SHORT 1\n");
    } else {
        printf("UCT_IFACE_FLAG_AM_SHORT 0\n");
    }

    if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_BCOPY) {
        printf("UCT_IFACE_FLAG_AM_BCOPY 1\n");
    } else {
        printf("UCT_IFACE_FLAG_AM_BCOPY 0\n");
    }

    if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_ZCOPY) {
        printf("UCT_IFACE_FLAG_AM_ZCOPY 1\n");
    } else {
        printf("UCT_IFACE_FLAG_AM_ZCOPY 0\n");
    }

    return UCS_OK;

}

static char *func_am_t_str(func_am_t func_am_type)
{
    switch (func_am_type) {
    case FUNC_AM_SHORT:
        return "uct_ep_am_short";
    case FUNC_AM_BCOPY:
        return "uct_ep_am_bcopy";
    case FUNC_AM_ZCOPY:
        return "uct_ep_am_zcopy";
    }
    return NULL;
}

static ucs_status_t hello_world(void *arg, void *data, size_t length,
                                unsigned flags)
{
    func_am_t func_am_type = *(func_am_t *)arg;
    // int *rdesc;

    printf("callback %s, %lu, %lu\n", func_am_t_str(func_am_type), (unsigned long) data, length);
}


int main(int argc, char **argv)
{
    ucs_async_context_t *async;
    uct_worker_h worker;

    /* Initialize context */
    ucs_status_t status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async);
    ERROR_CHECK_UCS_OK("ucs_async_context_create", status)

    /* Create a worker object */
    status = uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker);
    ERROR_CHECK_UCS_OK("uct_worker_create", status)

    uct_iface_attr_t    iface_attr; /* Interface attributes: capabilities and limitations */
    uct_iface_h         iface;      /* Communication interface context */
    uct_md_attr_t       md_attr;    /* Memory domain attributes: capabilities and limitations */
    uct_md_h            md;         /* Memory domain */

    /* Search for the desired transport */

    uct_tl_resource_desc_t *tl_res;

    status = dev_tl_lookup(&md, &md_attr, &tl_res);
    ERROR_CHECK_UCS_OK("dev_tl_lookup", status)

    status = init_iface(tl_res->dev_name, tl_res->tl_name, &iface_attr, &iface, md, worker);
    ERROR_CHECK_UCS_OK("init_iface", status)

    uint8_t id = 0;
    status = uct_iface_set_am_handler(iface, id, hello_world,
                                      FUNC_AM_SHORT, 0);
    ERROR_CHECK_UCS_OK("uct_iface_set_am_handler", status)

    /* Cleanup */
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
    return 0;
}
