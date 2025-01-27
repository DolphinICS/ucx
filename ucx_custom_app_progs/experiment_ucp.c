
#include <ucp/api/ucp.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <unistd.h>


struct my_ucx_context {
    int completed;
};

// Callback, not sure how to trigger it yet.  
static void request_init_callback(void *request)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;

    context->completed = 0;

    printf("Hello request_init_callback()\n");
}


int main(int argc, char **argv)
{
    printf("Hello! Starting program! experiment_ucp start!\n");

    ucs_status_t status;

    ucp_config_t *config;
    status = ucp_config_read(NULL, NULL, &config);


    // ucp_params -> ucp_init() -> ucp_context
    ucp_context_h ucp_context;
    ucp_params_t ucp_params = {};
    // ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES |
                              UCP_PARAM_FIELD_REQUEST_SIZE |
                              UCP_PARAM_FIELD_REQUEST_INIT |
                              UCP_PARAM_FIELD_NAME;
    ucp_params.features   = UCP_FEATURE_AM;

    // handle callback, function and it's parameter (request) size
    ucp_params.request_size = sizeof(struct my_ucx_context);
    ucp_params.request_init = request_init_callback;
    ucp_params.name = "hello_there";

    /* This function does open and query on the transports at least.
     * It results in printouts for the sisci transport component */
    // in --- uct_sci_md_open
    // in --- uct_sci_query_devices(uct_sci_iface_t, ...)
    status = ucp_init(&ucp_params, NULL, &ucp_context);
    assert(status == UCS_OK);

    // sleep(1);

    ucp_cleanup(ucp_context);

    printf("Goodbye! Exiting program! experiment_ucp end!\n");

    return 0;
}
