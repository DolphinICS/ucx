
#include <ucp/api/ucp.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

const char server_port_str[] = "59152";
sa_family_t ai_family    = AF_INET; // IPv4, AF_INET6 is IPv6, doesn't matter but good to know

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

    /* ----------------- Config Stuff ----------------- */
    ucp_config_t *config;
    status = ucp_config_read(NULL, NULL, &config);
    // // Prints a whole lot of environment variables, various default settings
    // // Small example snippet:
    // //    UCX_NET_DEVICES=all
    // //    UCX_SHM_DEVICES=all
    // //    UCX_ACC_DEVICES=all
    // //    UCX_SELF_DEVICES=all
    // ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);
    ucp_config_release(config);

    /* ----------------- ucp_init ----------------- */

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
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_init failed.\n");
        return EXIT_FAILURE;
    }
    // sleep(1);

    /* ----------------- ucp_worker ----------------- */

    // worker_params -> ucp_worker_create -> ucp_worker_h
    ucp_worker_h ucp_worker;
    ucp_worker_params_t worker_params = {};
    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
    // in --- uct_sci_query_devices(uct_sci_iface_t, ...)
    // in --- UCS_CLASS_INIT_FUNC(uct_sci_iface_t, ...)
    // in --- uct_sci_iface_query  * REPEATEDLY ...
    // in --- uct_sci_iface_progress_enable
    status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_worker_create failed.\n");
        return EXIT_FAILURE;
    }

    /* -- query attributes (may be some extra, non-essential stuff) -- */

    /* OOB connection vars (out of band connection),
     * apparently meaning control information outside of the primary data path
     * for setup etc. Used if you are implementing a higher level
     * communication API (like MPI), then you might use this for setup */
    uint64_t local_addr_len   = 0;
    ucp_address_t *local_addr = NULL;

    ucp_worker_attr_t worker_attr = {};
    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    status = ucp_worker_query(ucp_worker, &worker_attr);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_worker_query failed.\n");
        return EXIT_FAILURE;
    }
    local_addr_len = worker_attr.address_length;
    local_addr     = worker_attr.address;

    printf("[0x%x] local address length: %lu Bytes, name=%s\n",
           (unsigned int)pthread_self(), local_addr_len, worker_attr.name);

    int ret;

    if (argc > 1) {
        printf("Look at me. I am the server now\n");

        struct addrinfo hints = { 0 };
        struct addrinfo *res;
        struct addrinfo *ai_cur;

        hints.ai_flags    = AI_PASSIVE;
        hints.ai_family   = ai_family;
        hints.ai_socktype = SOCK_STREAM;

        ret = getaddrinfo(NULL, server_port_str, &hints, &res);
        if (ret < 0) {
            fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
            return EXIT_FAILURE;
        }
        printf("getaddrinfo successful\n");


    } else {
        printf("The client is always right\n");

        struct addrinfo hints = { 0 };
        struct addrinfo *res;
        struct addrinfo *ai_cur;
    }


    ucp_worker_destroy(ucp_worker); // ucp_worker_create cleanup
    ucp_cleanup(ucp_context); // ucp_init cleanup

    printf("Goodbye! Exiting program! experiment_ucp end!\n");

    return 0;
}
