
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

const char server_port_str[] = "59152";
sa_family_t ai_family    = AF_INET; // IPv4, AF_INET6 is IPv6, doesn't matter but good to know

static const ucp_tag_t tag      = 0x1337a880u;
static const ucp_tag_t tag_mask = UINT64_MAX;

static const char *addr_msg_str = "UCX address message";

static ucs_status_t ep_status   = UCS_OK;

ucp_err_handling_mode_t ucp_err_mode;


// Surely this can just be an int right? It looks like pointers to it should actually be a 
// typedef void* ucs_status_ptr_t, so it could maybe be of type uct_status, but an int could work.
// Still, this works for now, it just seems a bit overengineered for an example maybe.
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

// callback
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

// callback
static void failure_handler(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    ucs_status_t *arg_status = (ucs_status_t *)arg;

    printf("[0x%x] failure handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));

    *arg_status = status;
}

// callback
static void send_handler(void *request, ucs_status_t status, void *ctx)
{
    struct my_ucx_context *context = (struct my_ucx_context *)request;
    const char *str             = (const char *)ctx;

    context->completed = 1;

    printf("[0x%x] send handler called for \"%s\" with status %d (%s)\n",
           (unsigned int)pthread_self(), str, status,
           ucs_status_string(status));
}

static int run_ucx_server(ucp_worker_h ucp_worker) {

    printf("Server needs a function all to itself\n");

    ucp_tag_recv_info_t info_tag;
    ucp_tag_message_h msg_tag;

    /* ---------------- Receive the info tag... yes the info tag ---------------- */
    // Not sure what the info tag is used for, but apparently if you just probe the worker for it you will receive such an info tag.
    // I assume this happens when the client creates an endpoint, but let me see later.
    // Probably this can be used to set up some specific length of data, but the purpose is unclear right now.

    // Continuously update state and then probe for message.
    /* Receive client UCX address */
    do {
        /* Progressing before probe to update the state */

        // This routine explicitly progresses all communication operations on a worker.
        // Typically, request wait and test routines call this routine to progress any outstanding operations.
        // Done automatically for blocking routines? But not unblocking routines? Seems like it
        ucp_worker_progress(ucp_worker);

        /* Probing incoming events in non-block mode */

        // Checks if messages described by tag and tag_mask was received on worker.
        // tag is the message tag to probe for, tag_mask indicates relevant bits
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);

    // Hold on! maybe that is not true... I'm running in circles here. Hopefully I'll figure it out in the end
    // info_tag.length is basically the max size that can be sent or received with usp_tag_msg_****_nbx functions

    /* ---------------- Receive a message from client ---------------- */
    // As the rant below illustrates this seems unrelated to the info tag (to some degree)
    // At this point we can apparently just send messages as we like, no need to think of any info tags and such.
    // Don't know why it was used in the first place. I can experiment with some sizes to see if it has some purpose perhaps, that's todo
    // Okay, so I suppose the tag does have some function here, I was wrong. very wrong. I should investigate more

    // Okay, so this is really strange, there's this info_tag above, basically it contains the sender tag and a length which is the size of the received data
    // But what does this have anything to do with the message I am now receiving? That's kind of weird, seems unrelated why one would allocate space accomodating that
    // but then not receiving an info tag but instead just some random message that was sent using a message struct which is really just an int. Why all the layers.
    // Apparently I can just receive an int though without any fuss.
    // I don't need a struct to hold an int like the example does surely
    // // Allocate space for info_tag, I suppose that is the message.
    // // The message is the info? So the info_tag contains the length of the message? Sure, why not
    // struct msg *msg = malloc(info_tag.length);
    // if (msg == NULL) {
    //     fprintf(stderr, "malloc failed.\n");
    //     exit(EXIT_FAILURE);
    // }

    // This will be the message, I don't use the info tag right now, why use it?
    uint64_t message;

    // This is just some extra parameters to ucp_tag_msg_recv_nbx.
    // After all it's annoying to have too many parameters, so this is a struct to group them together that's all.
    ucp_request_param_t recv_param;
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
    recv_param.datatype     = ucp_dt_make_contig(1); // Contiguous datatype of size 1
    recv_param.cb.recv      = recv_handler;

    // Non blocking receive into msg buffer. Returns immediately, but! It's not really done! no no no, not before a message is in the buffer.
    // How do we know when a message is in the buffer? Oh well, for that we use a callback of course! Yes!
    // So in order to notify us about when the message is actually received and ready to be used,
    // ucx will call the callback function specified in recv_param. That way we can do some other stuff in the meantime. Right?
    struct my_ucx_context *request = ucp_tag_msg_recv_nbx(
        ucp_worker,
        &message, // void* buffer
        sizeof(uint64_t), // count, this used to be info_tag.length, but why? Why? That has nothing to do with sending a message it seems
        msg_tag, // ucp_tag_message_h message (message handler) !!!  This was obtained by ucp_tag_probe_nb() above!
        &recv_param); // const ucp_request_param_t ∗ param, a pointer to a struct, but is an input parameter clearly

    // Progress until completed, basically wait for ucp_worker's ucp_tag_msg_recv_nbx function
    while (!request->completed) {
        ucp_worker_progress(ucp_worker);
    }

    // Checks that it returns successfully, I mean it always does right? Nothing ever goes wrong
    ucs_status_t status = ucp_request_check_status(request);
    ucp_request_free(request);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_request_free failed.\n");
        exit(EXIT_FAILURE);
    }

    // Why all this mess? Well the example does lots of boilerplate and I tried to remove it, but you know, hacks, weird code, such is life
    printf("-**--**--** Server received message == %lu\n", message);

    // /* ---------------- again? ---------------- */

    // sleep(2);

    /* again? */

    // sleep(3); // with vs without sleep

    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
    recv_param.datatype     = ucp_dt_make_contig(1); // Contiguous datatype of size 1
    recv_param.cb.recv      = recv_handler;

    // const ucp_tag_t tag2      = 0x1337a880u;

    ucp_tag_message_h msg_tag2;

    do {
        /* Progressing before probe to update the state */

        // This routine explicitly progresses all communication operations on a worker.
        // Typically, request wait and test routines call this routine to progress any outstanding operations.
        // Done automatically for blocking routines? But not unblocking routines? Seems like it
        ucp_worker_progress(ucp_worker);

        /* Probing incoming events in non-block mode */

        // Checks if messages described by tag and tag_mask was received on worker.
        // tag is the message tag to probe for, tag_mask indicates relevant bits
        msg_tag2 = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
    } while (msg_tag2 == NULL);


    struct my_ucx_context *request2 = ucp_tag_msg_recv_nbx(
        ucp_worker,
        &message,
        sizeof(uint64_t),
        msg_tag2,
        &recv_param);

    while (!request2->completed) {
        ucp_worker_progress(ucp_worker);
    }

    status = ucp_request_check_status(request);
    ucp_request_free(request);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_request_free failed.\n");
        exit(EXIT_FAILURE);
    }

    // Why all this mess? Well the example does lots of boilerplate and I tried to remove it, but you know, hacks, weird code, such is life
    printf("-**--**--** Server received message == %lu\n", message);


    /* ---------------- Next step, receive bigger message ---------------- */

    // // message to receive
    // uint64_t big_message[10];
    // size_t big_message_size = 10 * sizeof(uint64_t);

    // for (uint64_t i = 0; i < 10; i++) {
    //     big_message[i] = 0;
    // }

    // request = ucp_tag_msg_recv_nbx(ucp_worker, big_message, 1, msg_tag, &recv_param);
    // // Wait until it completes
    // while (!request->completed) {
    //     ucp_worker_progress(ucp_worker);
    // }

    // // Checks that it returns successfully, I mean it always does right? Nothing ever goes wrong
    // status = ucp_request_check_status(request);
    // ucp_request_free(request);
    // if (status != UCS_OK) {
    //     fprintf(stderr, "PROGRAM ERROR! ucp_request_free failed.\n");
    //     exit(EXIT_FAILURE);
    // }

    // // receive things the dumb way
    // for (uint64_t i = 0; i < 10; i++) {
    //     request = ucp_tag_msg_recv_nbx(ucp_worker, big_message, big_message_size, msg_tag, &recv_param);
    //     // Wait until it completes
    //     while (!request->completed) {
    //         ucp_worker_progress(ucp_worker);
    //     }
    // }

    // printf("big_message = ");
    // for (int i = 0; i < 10; i++) {
    //     printf("%lu ", big_message[i]);
    // }
    // printf("\n");

}

static int blocking_flush(ucp_ep_h *server_ep, ucp_worker_h *ucp_worker) {
    ucp_request_param_t param;
    void *request;

    param.op_attr_mask = 0;
    request            = ucp_ep_flush_nbx(*server_ep, &param);
    if (request == NULL) {
        return UCS_OK;
    } else if (UCS_PTR_IS_ERR(request)) {
        return UCS_PTR_STATUS(request);
    } else {
        ucs_status_t status;
        do {
            ucp_worker_progress(*ucp_worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(request);
        return status;
    }
}

static int run_ucx_client(ucp_worker_h ucp_worker,
                          ucp_address_t *local_addr, size_t local_addr_len,
                          ucp_address_t *peer_addr, size_t peer_addr_len)
{
    printf("Client got its own function now\n");

    ucp_ep_params_t ep_params;

    /* Send client UCX address to server */
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address         = peer_addr;
    ep_params.user_data       = &ep_status;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;
    ep_params.err_mode        = ucp_err_mode; 


    // Handle errors? That's planning for failure!
    // ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
    //                             UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
    //                             UCP_EP_PARAM_FIELD_ERR_HANDLER |
    //                             UCP_EP_PARAM_FIELD_USER_DATA;
    // Desired error handling mode, optional parameter. Default value is UCP_ERR_HANDLING_MODE_NONE
    

    // Right, so the client treats the server as an endpoint
    ucp_ep_h server_ep;
    // creates and connects an endpoint on a local worker for a destination address that identifes the remote worker.
    // Non-blocking. Communication may begin immediately after it occurs. Endpoint associated with exactly one worker.
    ucs_status_t status = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_ep_create failed.\n");
        exit(EXIT_FAILURE);
    }

    // Right, so we got the peer_addr and peer_addr_len data from the server using what is called OOB (out of band) communication
    // Basically we just didn't use ucx, but used sockets instead for setup, cheating? I don't know, everyone does it man!
    // But now it looks like we are using ucx to send the same kind of information to the server, which is our endpoint.
    // That's basically what is going on in this first stage.
    // uint64_t message = local_addr_len;
    uint64_t message = 1234;
    size_t message_len = sizeof(message);

    // Some extra parameters, same type for send and receive
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = send_handler;
    send_param.user_data = (void*)addr_msg_str;
    // Sends a message, non-blocking
    struct my_ucx_context *request = ucp_tag_send_nbx(
        server_ep, // Destination endpoint handle
        &message, // buffer, pointer to message buffer (payload), what to send
        message_len, // Number of elements to send... bytes surely? Right? Must be bytes right?
        tag, // Tag, global variable, shared between client and server at start
        &send_param);

    printf("-**--**--** Client sends message == %lu\n", message);

    // Progress until completed, basically wait for ucp_worker's ucp_tag_send_nbx function
    while (!request->completed) {
        ucp_worker_progress(ucp_worker);
    }

    // Checks that it returns successfully, I mean it always does right? Nothing ever goes wrong
    status = ucp_request_check_status(request);
    ucp_request_free(request);
    if (status != UCS_OK) {
        fprintf(stderr, "PROGRAM ERROR! ucp_request_free failed.\n");
        exit(EXIT_FAILURE);
    }

    printf("-**--**--** Message sent successfully!\n");

    /* ------------------------- again ? -------------------------*/

    message = 22;

    // Okay!!! You need to flush the nbx before doing anymore transfers! Right!
    // Still there are some problems with segfaults if I forget to do this, but that's fine.
    // Maybe... That's the conclusion then. Always flush! Oh no... It returns... but I wasn't supposed to do that!
    blocking_flush(&server_ep, &ucp_worker);

    /* Sending twice seems to break things. No idea why. */

    // sleep(1); // with vs without sleep


    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = send_handler;
    send_param.user_data = (void*)addr_msg_str;

    request = ucp_tag_send_nbx(
        server_ep,
        &message,
        message_len,
        tag,
        &send_param);

    printf("-**--**--** Client sends message == %lu\n", message);

    // //
    // // Progress until completed, basically wait for ucp_worker's ucp_tag_send_nbx function
    // while (!request->completed) {
    //     ucp_worker_progress(ucp_worker);
    // }

    printf("-**--**--** Message sent successfully!\n");



    // status = ucx_wait(ucp_worker, request, "send",
    //                                    addr_msg_str);

    // /* ---------------- Next step, send bigger message ---------------- */
    // // not able to send a large message this way. Need a tag or something that allows more than uint64_t
    // // Probably need to use a whole other paradigm

    // // message to receive
    // uint64_t big_message[1000];
    // size_t big_message_size = 1000 * sizeof(uint64_t);

    // for (uint64_t i = 0; i < 1000; i++) {
    //     big_message[i] = i;
    // }

    // // Start receiving
    // request = ucp_tag_send_nbx(server_ep, big_message, 1, tag, &send_param);
    // // Wait until it completes
    // while (!request->completed) {
    //     ucp_worker_progress(ucp_worker);
    // }

    // // Checks that it returns successfully, I mean it always does right? Nothing ever goes wrong
    // status = ucp_request_check_status(request);
    // ucp_request_free(request);
    // if (status != UCS_OK) {
    //     fprintf(stderr, "PROGRAM ERROR! ucp_request_free failed.\n");
    //     exit(EXIT_FAILURE);
    // }

    // // for (uint64_t i = 0; i < 10; i++) {
    // //     // Start receiving
    // //     request = ucp_tag_send_nbx(server_ep, &big_message, big_message_size, tag, &send_param);
    // //     // Wait until it completes
    // //     while (!request->completed) {
    // //         ucp_worker_progress(ucp_worker);
    // //     }
    // //     big_message[i] = i;
    // // }

}

int main(int argc, char **argv)
{
    // For client arg[1] is server host name

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

    // handle callback, function and it's parameter (request) size
    ucp_params.request_size = sizeof(struct my_ucx_context);
    ucp_params.request_init = request_init_callback;
    ucp_params.name = "hello_there";

    // Woah, much later probe showed me this bug:
    // probe.c:28   UCX  ERROR feature flags UCP_FEATURE_TAG were not set for ucp_init()
    // guess ucp_init needs another flag!
    // UCP_FEATURE_TAG: Request tag matching support
    // Right and we're doing tag matching, of course we need it then! I must have forgot.
    // ucp_params.features   = UCP_FEATURE_AM; // this is not enough man
    ucp_params.features = UCP_FEATURE_AM | UCP_FEATURE_TAG;

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

    // Ok, so I guesss these are sent from server to client for some purpose or other
    local_addr_len = worker_attr.address_length;
    local_addr     = worker_attr.address;

    printf("[0x%x] local address length: %lu Bytes, name=%s\n",
           (unsigned int)pthread_self(), local_addr_len, worker_attr.name);

    int ret;

    bool is_server = false;

    if (argc < 2) {
        printf("Hey. ------------------------------------------- Look at me. I am the server now\n");
        is_server = true;
    } else {
        printf("Hey. ------------------------------------------- The client is always right\n");
    }

    // // How to get host endpoint from hostname, not really needed though,
    // // the hostname should be passed to getaddrinfo by the client as the first parameter
    // struct hostent *hp;
    // if (!is_server) {
    //     char *host = argv[1];
    //     hp = gethostbyname(host);
    //     if(!hp){
    //         fprintf(stderr, "unkown host: %s\n", host);
    //         exit(1);
    //     }
    //     printf("gethostbyname successful for host = %s\n", host);
    // }

    /* Important variable over here */
    int socket_fd;
    struct addrinfo *res;

    /* --------------------- Set up socket connection --------------------- */
    if (is_server) {
        struct addrinfo hints = { 0 };

        hints.ai_flags    = AI_PASSIVE;
        hints.ai_family   = ai_family;
        hints.ai_socktype = SOCK_STREAM;

        ret = getaddrinfo(NULL, server_port_str, &hints, &res);
        if (ret < 0) {
            fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
            return EXIT_FAILURE;
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
                return EXIT_FAILURE;
            }

            ret = bind(socket_fd, ai_cur->ai_addr, ai_cur->ai_addrlen);
            if (ret != 0) {
                perror("PROGRAM ERROR! bind failed\n");
                return EXIT_FAILURE;
            }

            ret = listen(socket_fd, 0);
            if (ret != 0) {
                perror("PROGRAM ERROR! listen failed\n");
                return EXIT_FAILURE;
            }

            fprintf(stdout, "Waiting for connection... Port is %s\n", server_port_str);
            int listen_fd = socket_fd;
            socket_fd = accept(listen_fd, NULL, NULL);
            close(listen_fd);

            // So socket_fd should now be open right? Do stuff before closing it
        }

    } else {
        struct addrinfo hints = { 0 };

        hints.ai_family   = ai_family;
        hints.ai_socktype = SOCK_STREAM;

        char *server_name = argv[1]; // which host to connect to, if left as NULL it defaults to the same computer apparently
        ret = getaddrinfo(server_name, server_port_str, &hints, &res);
        if (ret < 0) {
            fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
            return EXIT_FAILURE;
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
                return EXIT_FAILURE;
            }

        }
    }

    /* ------------------------ Just more setup, sending out of band info ------------------------ */

    // Gentlemen, we have the socket.
    // Server, send local_addr_len and local_addr (these are ucx worker attributes by the way!)
    // Client, get ready to receive!
    // I do not yet know the purpose of this, but it must be important, otherwise why do it?

    // Okay, apparently socket_fd is used to send OOB, i.e. out of band info.
    // You know, extra info that uses like a separate communication channel.

    uint64_t peer_addr_len    = 0;
    ucp_address_t *peer_addr  = NULL;

    if (is_server) {

        ret = send(socket_fd, &local_addr_len, sizeof(local_addr_len), 0);
        if (ret != (int)sizeof(local_addr_len)) {
            fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
            return EXIT_FAILURE;
        }
        printf("Sent local_addr_len with sockets, that's %d bytes btw!\n", ret);

        ret = send(socket_fd, local_addr, local_addr_len, 0);
        if (ret != (int)local_addr_len) {
            fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
            return EXIT_FAILURE;
        }
        printf("Sent local_addr with sockets, that's %d bytes btw!\n", ret);

    } else {

        ret = recv(socket_fd, &peer_addr_len, sizeof(peer_addr_len), MSG_WAITALL);
        if (ret != (int)sizeof(peer_addr_len)) {
            fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
            return EXIT_FAILURE;
        }
        printf("Received peer_addr_len with sockets, that's %d bytes btw!\n", ret);

        // You know... Ideally this would be cleaned up later in the program,
        // but this is just an example program. Don't be such a nerd.
        peer_addr = malloc(peer_addr_len);
        if (peer_addr == NULL) {
            fprintf(stderr, "malloc failed.\n");
            return EXIT_FAILURE;
        }

        ret = recv(socket_fd, peer_addr, peer_addr_len, MSG_WAITALL);
        if (ret != (int)peer_addr_len) {
            // perror("recv");
            fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
            return EXIT_FAILURE;
        }
        printf("Received peer_addr with sockets, that's %d bytes btw!\n", ret);
    }

    /* ----------------------------- Next step ----------------------------- */

    // Endpoints? Endpoints?
    // Love the mega function, but time to split into separate functions, really
    if (is_server) {
        run_ucx_server(ucp_worker);
    } else {
        run_ucx_client(ucp_worker, local_addr, local_addr_len, peer_addr, peer_addr_len);
    }


    /* --------------------- Tear down socket connection --------------------- */
    close(socket_fd);
    freeaddrinfo(res);


    ucp_worker_destroy(ucp_worker); // ucp_worker_create cleanup
    ucp_cleanup(ucp_context); // ucp_init cleanup

    printf("Goodbye! Exiting program! experiment_ucp end!\n");

    return 0;
}
