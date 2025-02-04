
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

static void request_init_callback(void *request)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;

    context->completed = 0;

    printf("Hello request_init_callback()\n");
}

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

static void run_ucx_server(ucp_worker_h ucp_worker) {
    /* Get local ucp address (server ucp address) */
    ucp_address_t *local_addr;
    uint64_t local_addr_len;
    get_ucp_addr(ucp_worker, &local_addr, &local_addr_len);
    /* Get send server ucp address to client */
    send_server_ucp_address(local_addr, local_addr_len);
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


}


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
    ucp_params.features = UCP_FEATURE_AM | UCP_FEATURE_TAG;

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
        run_ucx_server(ucp_worker);
    } else {
        run_ucx_client(ucp_worker, argv[1]);
    }

}
