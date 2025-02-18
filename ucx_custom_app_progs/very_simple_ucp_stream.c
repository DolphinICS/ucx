
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
// instead of being given to the ucp_stream_msg_recv_nbx function.
// Still the idea is clearly interaction between request_init_callback and recv_handler
// I believe this one is called by default in every call to request_init_callback?
static void request_init_callback(void *request)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;

    context->completed = 0;

    printf("Hello request_init_callback()\n");
}

// callback, called some time after ucp_stream_msg_recv_nbx when the message has successfully been received.
// Meaning the message should now be in the buffer that was sent to ucp_stream_msg_recv_nbx
static void recv_handler(void *request, ucs_status_t status, size_t length, void *user_data)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (%s)\n",
           (unsigned int)pthread_self(),
           status,
           ucs_status_string(status));
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

static void my_blocking_stream_send(ucp_worker_h ucp_worker, ucp_ep_h server_ep, void *message, size_t message_size) {
    
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = send_handler;
    static const char *addr_msg_str = "UCX address message";
    send_param.user_data = (void*)addr_msg_str;
    // Sends a message, non-blocking
    void *request = ucp_stream_send_nbx(
        server_ep, // Destination endpoint handle
        message, // buffer, pointer to message buffer (payload), what to send
        message_size, // Number of elements to send... bytes surely? Right? Must be bytes right?
        &send_param);


    // Check if request is NULL (completed immediately)
    if (request == NULL) {
        printf("Send completed immediately\n");
        return;
    }

    // Check for error status
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "PROGRAM ERROR! ucp_stream_send_nbx failed: %s\n", ucs_status_string(UCS_PTR_STATUS(request)));
        exit(EXIT_FAILURE);
    }
    
    struct my_ucx_context *ctx = (struct my_ucx_context *)request;
    while (!ctx->completed) {
        ucp_worker_progress(ucp_worker);
    }

    // ucs_status_t ep_status = ucp_request_check_status(server_ep);
    // printf("Endpoint status before second send: %s\n", ucs_status_string(ep_status));

    ucs_status_t status = ucp_request_check_status(request);
    ucp_request_free(request);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_request_free failed.\n");
        exit(EXIT_FAILURE);
    }

    blocking_flush(server_ep, ucp_worker);
}

static void my_blocking_stream_recv(ucp_worker_h ucp_worker, ucp_ep_h end_point, void *message, size_t message_size) {
    
    ucp_request_param_t recv_param;
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
    recv_param.op_attr_mask  |= UCP_OP_ATTR_FIELD_FLAGS;
    recv_param.flags          = UCP_STREAM_RECV_FLAG_WAITALL;
    recv_param.datatype     = ucp_dt_make_contig(1); // Contiguous datatype of size 1
    recv_param.cb.recv_stream      = recv_handler;

    struct my_ucx_context *request = ucp_stream_recv_nbx(
        end_point,
        message, // void* buffer
        message_size, // Length of message
        &message_size, // The message stream which we received by our helper function get_msg_stream which probed for it using ucp_stream_probe_nb 
        &recv_param);

    // Wait until message is received. Our callback sets the completed variable to 1
    while (!request->completed) {
        ucp_worker_progress(ucp_worker);
    }

}

static void run_ucx_server(ucp_worker_h ucp_worker, char *client_hostname) {
    /* Get local ucp address (server ucp address) */
    ucp_address_t *local_addr;
    uint64_t local_addr_len;
    get_ucp_addr(ucp_worker, &local_addr, &local_addr_len);
    /* Get send server ucp address to client */
    send_server_ucp_address(local_addr, local_addr_len);
    printf("Successfully sent ucp address to server\n");

    ucp_address_t *peer_addr;
    size_t peer_addr_len;
    /* Get peer ucp address */
    receive_server_ucp_address(client_hostname, &peer_addr, &peer_addr_len);
    printf("Successfully received ucp address from client\n");

    // ---- Create emnd point. End points are needed rather than workers when receiving streams apparently

    ucp_err_handling_mode_t ucp_err_mode = UCP_ERR_HANDLING_MODE_PEER;

    static ucs_status_t ep_status = UCS_OK;
    ucp_ep_params_t ep_params;
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address         = peer_addr;
    ep_params.err_mode        = ucp_err_mode;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;
    ep_params.user_data       = &ep_status;

    ucp_ep_h server_ep;
    ucs_status_t status = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_ep_create failed.\n");
        exit(EXIT_FAILURE);
    }


    /* -- Server done with addresses -- */
    printf("Successfully sent ucp address to client\n");

    /* So instead we'll assume the size to be eight bytes */
    uint64_t message;
    my_blocking_stream_recv(ucp_worker, server_ep, &message, sizeof(message));
    printf("Received message: %lu\n", message);

    my_blocking_stream_recv(ucp_worker, server_ep, &message, sizeof(message));
    printf("Received message: %lu\n", message);
    
    my_blocking_stream_recv(ucp_worker, server_ep, &message, sizeof(message));
    printf("Received message: %lu\n", message);

    uint32_t msg2[10] = { 0 };

    my_blocking_stream_recv(ucp_worker, server_ep, msg2, sizeof(msg2[0]) * 10);
    printf("Received message: ");
    for (int i = 0; i < 10; i++) {
        printf("%u ", msg2[i]);
    }
    printf("\n");


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
    printf("Successfully received ucp address from server\n");

    send_server_ucp_address(local_addr, local_addr_len);
    printf("Successfully sent ucp address to server\n");

    /* -- Client done with addresses -- */

    // UCP_ERR_HANDLING_MODE_NONE saves resources.
    // UCP_ERR_HANDLING_MODE_PEER means more error handling. I assume without this program having to do any extra setup.
    // c                          comes at the coost of some performance etc. But this is an example.
    ucp_err_handling_mode_t ucp_err_mode = UCP_ERR_HANDLING_MODE_PEER;

    static ucs_status_t ep_status = UCS_OK;
    ucp_ep_params_t ep_params;
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address         = peer_addr;
    ep_params.err_mode        = ucp_err_mode;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;
    ep_params.user_data       = &ep_status;

    ucp_ep_h server_ep;
    ucs_status_t status = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_ep_create failed.\n");
        exit(EXIT_FAILURE);
    }

    uint64_t message = 1234;
    my_blocking_stream_send(ucp_worker, server_ep, &message, sizeof(message));
    printf("Sent message %lu, (%lu bytes)\n", message, sizeof(message));

    message = 5500550055;
    my_blocking_stream_send(ucp_worker, server_ep, &message, sizeof(message));
    printf("Sent message %lu, (%lu bytes)\n", message, sizeof(message));
    
    message = 4444477777;
    my_blocking_stream_send(ucp_worker, server_ep, &message, sizeof(message));
    printf("Sent message %lu, (%lu bytes)\n", message, sizeof(message));

    uint32_t msg2[10];
    for (uint32_t i = 0; i < 10; i++) {
        msg2[i] = i;
    }
    my_blocking_stream_send(ucp_worker, server_ep, msg2, sizeof(msg2[0]) * 10);

}


int main(int argc, char **argv)
{

    bool is_server = false;
    if (argc < 3) {
        printf("This is a server now. In this program, no arguments means server\n");
        is_server = true;
    } else {
        printf("This is a client. You gave two arguments so it's a client\n");
        printf("Hopefully the argument is the hostname of the server, because that's what I'm expecting.\n");
    }

    ucs_status_t status;

    ucp_context_h ucp_context;
    ucp_params_t ucp_params = {};
    // ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES |
                              UCP_PARAM_FIELD_REQUEST_SIZE |
                              UCP_PARAM_FIELD_REQUEST_INIT |
                              UCP_PARAM_FIELD_NAME;

    // handle callback, function and it's parameter (request) size
    ucp_params.request_size = sizeof(struct my_ucx_context);
    ucp_params.request_init = request_init_callback;
    ucp_params.name = "hello_there";
    ucp_params.features = UCP_FEATURE_STREAM;

    status = ucp_init(&ucp_params, NULL, &ucp_context);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_init failed.\n");
        return EXIT_FAILURE;
    }

    ucp_worker_h ucp_worker;
    ucp_worker_params_t worker_params = {};
    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
    status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_worker_create failed.\n");
        return EXIT_FAILURE;
    }

    if (is_server) {
        run_ucx_server(ucp_worker, argv[1]);
    } else {
        run_ucx_client(ucp_worker, argv[1]);
    }

}
