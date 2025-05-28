#include <uct/api/uct.h>
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

    // for (int cmpt_index = 0; cmpt_index < num_components; ++cmpt_index) {

    //     uct_component_attr_t   component_attr;

    //     component_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCE_COUNT;
    //     status = uct_component_query(components[cmpt_index], &component_attr);
    //     ERROR_CHECK_UCS_OK("uct_query_components", status)
    // }
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

    // /* Search for the desired transport */
    // status = dev_tl_lookup(&cmd_args, &if_info);
    // assert(status == UCS_OK);

    /* Cleanup */
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
    return 0;
}
