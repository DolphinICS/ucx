
#include <ucp/api/ucp.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

struct my_ucx_context {
    int completed;
};

// Callback. Kind of initial state before our callback. This one seems to be tied to our ucp_context
// instead of being given to the ucp_tag_msg_recv_nbx function.
// Still the idea is clearly interaction between request_init_callback and recv_handler
// I believe this one is called by default in every call to request_init_callback?
static void request_init_callback(void *request)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;

    context->completed = 0;

    printf("Hello request_init_callback()\n");
}

// callback, called some time after ucp_tag_msg_recv_nbx when the message has successfully been received.
// Meaning the message should now be in the buffer that was sent to ucp_tag_msg_recv_nbx
static void recv_handler(void *request, ucs_status_t status,
                         const ucp_tag_recv_info_t *info, void *user_data)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (%s), length %lu\n",
           (unsigned int)pthread_self(),
           status,
           ucs_status_string(status),
           info->length);
}

static void failure_handler(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    ucs_status_t *arg_status = (ucs_status_t *)arg;

    printf("[0x%x] failure handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));

    *arg_status = status;
}

static void send_handler(void *request, ucs_status_t status, void *ctx)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;
    const char *str             = (const char *)ctx;

    context->completed = 1;

    printf("[0x%x] send handler called for \"%s\" with status %d (%s)\n",
           (unsigned int)pthread_self(), str, status,
           ucs_status_string(status));
}

static void get_ucp_addr(ucp_worker_h ucp_worker, ucp_address_t **local_addr, uint64_t *local_addr_len) {
    ucs_status_t status;
    ucp_worker_attr_t worker_attr = {};
    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    status = ucp_worker_query(ucp_worker, &worker_attr);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_worker_query failed.\n");
        exit(EXIT_FAILURE);
    }

    *local_addr_len = worker_attr.address_length;
    *local_addr = worker_attr.address;
}

/* ---------------- Need to use other communication system to send peer address ---------------- */

const char server_port_str[] = "59152";

static void receive_server_ucp_address(char *server_hostname, ucp_address_t **peer_addr, size_t *peer_addr_len) {
    int socket_fd;
    struct addrinfo *res;
    int ret;

    struct addrinfo hints = { 0 };

    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* --- set up socket connection with server --- */

    ret = getaddrinfo(server_hostname, server_port_str, &hints, &res);
    if (ret < 0) {
        fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    printf("getaddrinfo successful\n");

    int cnt = 0; // just for printouts

    printf("Iterating through addrinfo structs\n");
    for (struct addrinfo *ai_cur = res; ai_cur != NULL; ai_cur = ai_cur->ai_next) {

        printf("* iteration %d\n", cnt);

        socket_fd = socket(ai_cur->ai_family, ai_cur->ai_socktype, ai_cur->ai_protocol);
        if (socket_fd < 0) {
            printf("socket failed here, moving on!\n");
            continue;
        }
        
        fprintf(stdout, "Trying to connect to server on port %s\n", server_port_str);
        ret = connect(socket_fd, ai_cur->ai_addr, ai_cur->ai_addrlen);
        if (ret != 0) {
            perror("PROGRAM ERROR! connect failed\n");
            exit(EXIT_FAILURE);
        }
    }

    /* --- send ucp addr info using sockets --- */

    ret = recv(socket_fd, peer_addr_len, sizeof(size_t), MSG_WAITALL);
    if (ret != (int)sizeof(size_t)) {
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Received peer_addr_len with sockets, that's %d bytes btw!\n", ret);

    // I'm not gonna free this ever. Why? Why should I? You can't tell me what to do!
    // You're not my... wait, are you my boss? Look, this isn't what it looks like
    *peer_addr = malloc(*peer_addr_len);
    if (*peer_addr == NULL) {
        fprintf(stderr, "malloc failed.\n");
        exit(EXIT_FAILURE);
    }

    ret = recv(socket_fd, *peer_addr, *peer_addr_len, MSG_WAITALL);
    if (ret != (int)*peer_addr_len) {
        // perror("recv");
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Received peer_addr with sockets, that's %d bytes btw!\n", ret);

    /* --- Free resources --- */
    close(socket_fd);
    freeaddrinfo(res);
}

static void send_server_ucp_address(ucp_address_t *local_addr, size_t local_addr_len) {
    int socket_fd;
    struct addrinfo *res;
    int ret;

    struct addrinfo hints = { 0 };

    hints.ai_flags    = AI_PASSIVE;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* --- set up socket connection with client --- */

    ret = getaddrinfo(NULL, server_port_str, &hints, &res);
    if (ret < 0) {
        fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    printf("getaddrinfo successful\n");

    int cnt = 0; // just for printouts

    printf("Iterating through addrinfo structs\n");
    for (struct addrinfo *ai_cur = res; ai_cur != NULL; ai_cur = ai_cur->ai_next) {

        printf("* iteration %d\n", cnt);

        socket_fd = socket(ai_cur->ai_family, ai_cur->ai_socktype, ai_cur->ai_protocol);
        if (socket_fd < 0) {
            printf("socket failed here, moving on!\n");
            continue;
        }
        
        // Set up server port or whatever and wait for connections
        int optval;
        ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                            sizeof(optval));
        if (ret < 0) {
            perror("PROGRAM ERROR! setsockopt failed\n");
            exit(EXIT_FAILURE);
        }

        ret = bind(socket_fd, ai_cur->ai_addr, ai_cur->ai_addrlen);
        if (ret != 0) {
            perror("PROGRAM ERROR! bind failed\n");
            exit(EXIT_FAILURE);
        }

        ret = listen(socket_fd, 0);
        if (ret != 0) {
            perror("PROGRAM ERROR! listen failed\n");
            exit(EXIT_FAILURE);
        }

        fprintf(stdout, "Waiting for connection... Port is %s\n", server_port_str);
        int listen_fd = socket_fd;
        socket_fd = accept(listen_fd, NULL, NULL);
        close(listen_fd);

        // So socket_fd should now be open right
    }

    /* --- send ucp addr info using sockets --- */

    ret = send(socket_fd, &local_addr_len, sizeof(local_addr_len), 0);
    if (ret != (int)sizeof(local_addr_len)) {
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Sent local_addr_len with sockets, that's %d bytes btw!\n", ret);

    ret = send(socket_fd, local_addr, local_addr_len, 0);
    if (ret != (int)local_addr_len) {
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }
    printf("Sent local_addr with sockets, that's %d bytes btw!\n", ret);

    /* --- Free resources --- */
    close(socket_fd);
    freeaddrinfo(res);
}

/* ---------------------  Done with other comm system to send peer address ---------------------  */


static const ucp_tag_t my_tag      = 0x1337a880u;
static const ucp_tag_t my_tag_mask = UINT64_MAX;

static int blocking_flush(ucp_ep_h server_ep, ucp_worker_h ucp_worker) {
    ucp_request_param_t param;
    void *request;

    param.op_attr_mask = 0;
    request            = ucp_ep_flush_nbx(server_ep, &param);
    if (request == NULL) {
        printf("blocking_flush - request == NULL\n");
        return UCS_OK;
    } else if (UCS_PTR_IS_ERR(request)) {
        printf("blocking_flush - UCS_PTR_IS_ERR(request) != 0\n");
        return UCS_PTR_STATUS(request);
    } else {
        printf("blocking_flush - else\n");
        ucs_status_t status;
        do {
            ucp_worker_progress(ucp_worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(request);
        return status;
    }
}

#define AM_MSG_ID 0

static void my_blocking_am_send(ucp_worker_h ucp_worker, ucp_ep_h server_ep, void *message, size_t message_size) {
    

}

static void my_blocking_am_recv(ucp_worker_h ucp_worker, void *message, size_t message_size) {
    
    
}

bool message_received = false;

static ucs_status_t my_am_recv_handler(void *arg, const void *header,
                                       size_t header_length, void *data,
                                       size_t length,
                                       const ucp_am_recv_param_t *param) {
    printf("Received Active Message: %zu bytes\n", length);
    message_received = true;
    return UCS_OK; // Tell UCX we're done
}

static void run_ucx_server(ucp_worker_h ucp_worker) {
    /* Get local ucp address (server ucp address) */
    ucp_address_t *local_addr;
    uint64_t local_addr_len;
    get_ucp_addr(ucp_worker, &local_addr, &local_addr_len);
    /* Get send server ucp address to client */
    send_server_ucp_address(local_addr, local_addr_len);

    ucp_am_handler_param_t am_param = {0};
    am_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                          UCP_AM_HANDLER_PARAM_FIELD_CB;
    am_param.id         = AM_MSG_ID; // AM ID (just use 0 for now)
    am_param.cb         = my_am_recv_handler; // Handler function

    ucs_status_t status = ucp_worker_set_am_recv_handler(ucp_worker, &am_param);
    if (status != UCS_OK) {
        fprintf(stderr, "Failed to set AM handler: %s\n", ucs_status_string(status));
        exit(EXIT_FAILURE);
    }

    while (!message_received) {
        ucp_worker_progress(ucp_worker);  // Wait for incoming messages.
    }

    sleep(1);
}

static void run_ucx_client(ucp_worker_h ucp_worker, char *server_hostname){

    /* Get local ucp address (client ucp address) */
    ucp_address_t *local_addr;
    uint64_t local_addr_len;
    get_ucp_addr(ucp_worker, &local_addr, &local_addr_len);

    ucp_address_t *peer_addr;
    size_t peer_addr_len;
    /* Get peer ucp address */
    receive_server_ucp_address(server_hostname, &peer_addr, &peer_addr_len);

    /* -- Client done with addresses -- */
    printf("Successfully received ucp address from server\n");
    ucp_ep_h server_ep;

    ucp_request_param_t send_param = {0};
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_DATATYPE | UCP_OP_ATTR_FIELD_USER_DATA | UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
    send_param.cb.send = send_handler; // Callback when send completes

    // static uint64_t message = 1234;
    uint64_t *message = malloc(sizeof(uint64_t) * 100);
    *message = 1234;
    // void *unused_header = malloc(10000);
    void *request = ucp_am_send_nbx(server_ep, AM_MSG_ID, NULL, 0, &message, sizeof(uint64_t), &send_param);
    blocking_flush(server_ep, ucp_worker);


}


/**
 * Check if at least one feature flag from @a _flags is initialized.
 */
// #define UCP_CONTEXT_CHECK_FEATURE_FLAGS(_context, _flags, _action) \
//     do { \
//         if (ENABLE_PARAMS_CHECK && \
//             ucs_unlikely(!((_context)->config.features & (_flags)))) {  \
//             size_t feature_list_str_max = 512; \
//             char *feature_list_str = ucs_alloca(feature_list_str_max);  \
//             ucs_error("feature flags %s were not set for ucp_init()", \
//                       ucs_flags_str(feature_list_str, feature_list_str_max,  \
//                                     (_flags) & ~(_context)->config.features, \
//                                     ucp_feature_str)); \
//             _action; \
//         } \
//     } while (0)


// #include <ucp_context.h>

int main(int argc, char **argv)
{

    bool is_server = false;
    if (argc < 2) {
        printf("This is a server now. In this program, no arguments means server\n");
        is_server = true;
    } else {
        printf("This is a client. You gave an argument so it's a client\n");
        printf("Hopefully the argument is the hostname of the server, because that's what I'm expecting.\n");
    }

    ucs_status_t status;
    ucp_config_t *config;

    // Okay... Config field shouldn't be NULL in ucp_init I think. Need to read config then. That fixes one error at least.
    status = ucp_config_read(NULL, NULL, &config);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_config_read failed\n");
        return EXIT_FAILURE;
    }

    ucp_context_h ucp_context;
    ucp_params_t ucp_params = {};
    memset(&ucp_params, 0, sizeof(ucp_params));
    // ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_FEATURE_EXPORTED_MEMH;  // UCP_FEATURE_EXPORTED_MEMH is used in perftest
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;

    // ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES |
    //                           UCP_PARAM_FIELD_REQUEST_SIZE |
    //                           UCP_PARAM_FIELD_REQUEST_INIT;

    // handle callback, function and it's parameter (request) size
    // ucp_params.request_size = sizeof(struct my_ucx_context);
    // ucp_params.request_init = request_init_callback;
    ucp_params.features = UCP_FEATURE_AM;

    status = ucp_init(&ucp_params, config, &ucp_context);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_init failed.\n");
        return EXIT_FAILURE;
    }
    printf("Requested UCP features: 0x%lx\n", ucp_params.features);

    ucp_worker_h ucp_worker;
    ucp_worker_params_t worker_params = {};
    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
    status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_worker_create failed.\n");
        return EXIT_FAILURE;
    }


    // struct ucp_context *context_ptr = (struct ucp_context *) ucp_context;
    // if (!(context_ptr->config.features & UCP_FEATURE_AM)) {
    //     fprintf(stderr, "ERROR: UCP_FEATURE_AM is NOT set in ucp_context!\n");
    // } else {
    //     printf("SUCCESS: UCP_FEATURE_AM is set in ucp_context!\n");
    // }

    // if (!(ucp_context->config.features & UCP_FEATURE_AM)) {
    //     fprintf(stderr, "ERROR: UCP_FEATURE_AM is NOT set in ucp_context!\n");
    // } else {
    //     printf("SUCCESS: UCP_FEATURE_AM is set in ucp_context!\n");
    // }


    // UCP_CONTEXT_CHECK_FEATURE_FLAGS(worker->context, UCP_FEATURE_AM,
    //                                 fprintf(stderr, "UCP_CONTEXT_CHECK_FEATURE_FLAGS failed"));


    if (is_server) {
        run_ucx_server(ucp_worker);
    } else {
        run_ucx_client(ucp_worker, argv[1]);
    }
}
