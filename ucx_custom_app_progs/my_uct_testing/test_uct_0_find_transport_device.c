#include <uct/api/uct.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define ERROR_CHECK_UCS_OK(func_name, error) \
if (error != UCS_OK) {  \
    fprintf(stderr, "Error %s, line %u: %s failed, error code %d", __FUNCTION__, __LINE__, func_name, (int)error); \
    exit(EXIT_FAILURE); \
}

static ucs_status_t dev_tl_lookup()
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
    uct_md_h md;
    uct_md_attr_t md_attr;

    int eno1_index = -1;

    /* Iterate through memory domain resources */
    for (int md_index = 0; md_index < component_attr.md_resource_count; ++md_index) {

        printf("md_index == %d\n", md_index);

        uct_md_config_t *md_config;
        status = uct_md_config_read(components[tcp_index], NULL, NULL,
                                        &md_config);
        ERROR_CHECK_UCS_OK("uct_md_config_read", status)
        status = uct_md_open(components[tcp_index],
                                 component_attr.md_resources[md_index].md_name,
                                 md_config, &md);
        uct_config_release(md_config);
        ERROR_CHECK_UCS_OK("uct_md_open", status)

        status = uct_md_query(md, &md_attr);
        ERROR_CHECK_UCS_OK("uct_md_query", status)

        uct_tl_resource_desc_t *tl_resources = NULL;
        unsigned int num_tl_resources;
        status = uct_md_query_tl_resources(md, &tl_resources,
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


    return UCS_OK;
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

    /* Search for the desired transport */
    status = dev_tl_lookup();
    ERROR_CHECK_UCS_OK("dev_tl_lookup", status)

    /* Cleanup */
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
    return 0;
}
