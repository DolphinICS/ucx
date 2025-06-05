#include <uct/api/uct.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>



#define ERROR_CHECK_UCS_OK(func_name, error) \
if (error != UCS_OK) {  \
    fprintf(stderr, "Error %s, line %u: %s failed, error code %d\n", __FUNCTION__, __LINE__, func_name, (int)error); \
    exit(EXIT_FAILURE); \
}

#define ERROR_CHECK_ZERO(func_name, error) \
if (error != 0) {  \
    fprintf(stderr, "Error %s, line %u: %s failed, error code %d\n", __FUNCTION__, __LINE__, func_name, (int)error); \
    exit(EXIT_FAILURE); \
}

#define AM_ID 0

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

    int pcie_tl_cmp_index = -1;

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
        if (strncmp(component_attr.name, "pcie", 4) == 0) {
            pcie_tl_cmp_index = cmpt_index;
        }
    }

    printf("pcie_tl_cmp_index == %d\n", pcie_tl_cmp_index);

    int pcie_dev_index = -1;

    uct_tl_resource_desc_t *tl_resources = NULL;
    /* Iterate through memory domain resources */
    for (int md_index = 0; md_index < component_attr.md_resource_count; ++md_index) {

        printf("md_index == %d\n", md_index);

        uct_md_config_t *md_config;
        status = uct_md_config_read(components[pcie_tl_cmp_index], NULL, NULL,
                                        &md_config);
        ERROR_CHECK_UCS_OK("uct_md_config_read", status)
        status = uct_md_open(components[pcie_tl_cmp_index],
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

            if (strncmp(tl_resources[tl_index].dev_name, "pcie", 4) == 0) {
                pcie_dev_index = tl_index;
            }
        }
    }

    printf("pcie_dev_index == %d\n", pcie_dev_index);

    if (tl_resources != NULL) {
        *tl_res = &tl_resources[pcie_dev_index];
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

    // other thing:

    if (iface_attr->cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP) {
        printf("UCT_IFACE_FLAG_CONNECT_TO_EP 1\n");
    } else {
        printf("UCT_IFACE_FLAG_CONNECT_TO_EP 0\n");
    }

    if (iface_attr->cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE) {
        printf("UCT_IFACE_FLAG_CONNECT_TO_IFACE 1\n");
    } else {
        printf("UCT_IFACE_FLAG_CONNECT_TO_IFACE 0\n");
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

int callback_var = 0;

static ucs_status_t hello_world(void *arg, void *data, size_t length,
                                unsigned flags)
{
    func_am_t func_am_type = *(func_am_t *)arg;
    // int *rdesc;
    fprintf(stderr, "------------------- caaaaaaaaaaaaaaalbaaaaaaaaaaaaaaaaack ----------------");

    printf("callback %s, %lu, %lu\n", func_am_t_str(func_am_type), (unsigned long) data, length);

    callback_var++;
}

const char server_port_str[] = "59152";


int server_connect_to_client() {

    int socket_fd = 0;

    int ret;

    struct addrinfo hints = { 0 };
    hints.ai_flags    = AI_PASSIVE;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res;


    ret = getaddrinfo(NULL, server_port_str, &hints, &res);
    if (ret < 0) {
        fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    printf("getaddrinfo successful\n");

    printf("Iterating through addrinfo structs\n");
    for (struct addrinfo *ai_cur = res; ai_cur != NULL; ai_cur = ai_cur->ai_next) {

        printf("* Loop, hello\n");

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
        if (socket_fd < 0) {
            perror("PROGRAM ERROR! accept failed\n");
        }
        close(listen_fd);

        // So socket_fd should now be open
    }
    
    return socket_fd;
}


int client_connect_to_server(char *server_hostname) {

    printf("client_connect_to_server start\n");

    int socket_fd;

    struct addrinfo hints = { 0 };
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res;

    int ret = getaddrinfo(server_hostname, server_port_str, &hints, &res);
    if (ret < 0) {
        fprintf(stderr, "PROGRAM ERROR! getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    printf("getaddrinfo successful\n");

    for (struct addrinfo *ai_cur = res; ai_cur != NULL; ai_cur = ai_cur->ai_next) {

        printf("* Loop, hello\n");

        socket_fd = socket(ai_cur->ai_family, ai_cur->ai_socktype, ai_cur->ai_protocol);
        if (socket_fd < 0) {
            printf("socket failed here, moving on!\n");
            continue;
        }
        
        printf("Trying to connect to server on port %s\n", server_port_str);
        
        ret = connect(socket_fd, ai_cur->ai_addr, ai_cur->ai_addrlen);
        if (ret != 0) {
            perror("PROGRAM ERROR! connect failed\n");
            exit(EXIT_FAILURE);
        }
    }

    printf("client_connect_to_server stop\n");

    return socket_fd;
}

void receive_dev_and_iface(
    int socket_fd,
    uct_device_addr_t **peer_dev,
    size_t *peer_dev_len,
    uct_iface_addr_t **peer_iface,
    size_t *peer_iface_len)
{
    int ret;

    printf("Receiving peer_dev_len\n");
    ret = recv(socket_fd, peer_dev_len, sizeof(size_t), MSG_WAITALL);
    if (ret != (int)sizeof(size_t)) {
        perror("recv");
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }

    *peer_dev = calloc(1, *peer_dev_len);
    if (*peer_dev == NULL) {
        perror("recv");
        fprintf(stderr, "Failed to allocate peer_dev\n");
        exit(EXIT_FAILURE);
    }

    printf("Receiving peer_dev\n");
    ret = recv(socket_fd, *peer_dev, *peer_dev_len, MSG_WAITALL);
    if (ret != (int)*peer_dev_len) {
        perror("recv");
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }


    printf("Receiving peer_iface_len\n");
    ret = recv(socket_fd, peer_iface_len, sizeof(size_t), MSG_WAITALL);
    if (ret != (int)sizeof(size_t)) {
        perror("recv");
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }

    *peer_iface = calloc(1, *peer_iface_len);
    if (*peer_iface == NULL) {
        perror("calloc");
        fprintf(stderr, "Failed to allocate peer_iface\n");
        exit(EXIT_FAILURE);
    }

    printf("Receiving peer_iface\n");
    ret = recv(socket_fd, *peer_iface, *peer_iface_len, MSG_WAITALL);
    if (ret != (int)*peer_iface_len) {
        perror("recv");
        fprintf(stderr, "recv failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }
}

void send_dev_and_iface(
    int socket_fd,
    uct_device_addr_t *own_dev,
    size_t own_dev_len,
    uct_iface_addr_t *own_iface,
    size_t own_iface_len)
{
    int ret;

    printf("Sending own_dev_len\n");
    ret = send(socket_fd, &own_dev_len, sizeof(size_t), 0);
    if (ret != (int)sizeof(size_t)) {
        perror("send");
        fprintf(stderr, "send failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }

    printf("Sending own_dev\n");
    ret = send(socket_fd, own_dev, own_dev_len, 0);
    if (ret != (int)own_dev_len) {
        perror("send");
        fprintf(stderr, "send failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }

    printf("Sending own_iface_len\n");
    ret = send(socket_fd, &own_iface_len, sizeof(size_t), 0);
    if (ret != (int)sizeof(size_t)) {
        perror("send");
        fprintf(stderr, "send failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }

    printf("Sending own_iface\n");
    ret = send(socket_fd, own_iface, own_iface_len, 0);
    if (ret != (int)own_iface_len) {
        perror("send");
        fprintf(stderr, "send failed. %d bytes received, should have been higher\n", ret);
        exit(EXIT_FAILURE);
    }
}


void run_ucx_server(
    uct_worker_h worker,
    uct_device_addr_t *own_dev,
    size_t own_dev_len,
    uct_iface_addr_t *own_iface,
    size_t own_iface_len,
    uct_iface_h iface)
{
    printf("Yes, yes, this is the server.\n");

    ucs_status_t status;
    int ret;

    int socket_fd = server_connect_to_client();

    uct_device_addr_t *peer_dev;
    size_t peer_dev_len;
    uct_iface_addr_t *peer_iface;
    size_t peer_iface_len;
    receive_dev_and_iface(socket_fd, &peer_dev, &peer_dev_len, &peer_iface, &peer_iface_len);

    send_dev_and_iface(socket_fd, own_dev, own_dev_len, own_iface, own_dev_len);

    int is_reachable = uct_iface_is_reachable(iface, peer_dev, peer_iface);
    if (is_reachable == 0) {
        fprintf(stderr, "uct_iface_is_reachable returned false. Peer is not reachable for this iface\n");
        exit(EXIT_FAILURE);
    }
    printf("uct_iface_is_reachable returned true\n");

    uct_ep_params_t ep_params;
    ep_params.field_mask = UCT_EP_PARAM_FIELD_IFACE |
                           UCT_EP_PARAM_FIELD_DEV_ADDR |
                           UCT_EP_PARAM_FIELD_IFACE_ADDR;
    ep_params.iface = iface;
    ep_params.dev_addr    = peer_dev;
    ep_params.iface_addr  = peer_iface;

    uct_ep_h ep;
    status = uct_ep_create(&ep_params, &ep);
    ERROR_CHECK_UCS_OK("uct_ep_create", status)
    printf("uct_ep_create succeeded\n");

    char *str = "hello";
    printf("string on server side: %s\n", str);

    // do {
    //     /* Send active message to remote endpoint */
    //     // status = uct_ep_am_short(ep, AM_ID, 1ULL, str, strlen(str) + 8);
    //     uct_worker_progress(worker);
    // } while (status == UCS_ERR_NO_RESOURCE);

    for (int i = 0; i < 100; i++) {
        status = uct_ep_am_short(ep, AM_ID, 1ULL, str, strlen(str) + 8);
        uct_worker_progress(worker);
        printf("status == %d\n", (int) status);
        if (status != UCS_ERR_NO_RESOURCE) {
            printf("uct_ep_am_short succeeded on %i'th try\n", i);
            break;
        }
    }
    if (status == UCS_ERR_NO_RESOURCE) {
        printf("Tried a hundred times and status is still UCS_ERR_NO_RESOURCE\n");
    }

    printf("something was sent ?\n");

    close(socket_fd);
}

void run_ucx_client(
    char *server_name,
    uct_worker_h worker,
    uct_device_addr_t *own_dev,
    size_t own_dev_len,
    uct_iface_addr_t *own_iface,
    size_t own_iface_len,
    uct_iface_h iface)
{
    printf("Okay, okay, this is the client.\n");

    ucs_status_t status;

    int socket_fd = client_connect_to_server(server_name);
    
    send_dev_and_iface(socket_fd, own_dev, own_dev_len, own_iface, own_dev_len);

    uct_device_addr_t *peer_dev;
    size_t peer_dev_len;
    uct_iface_addr_t *peer_iface;
    size_t peer_iface_len;
    receive_dev_and_iface(socket_fd, &peer_dev, &peer_dev_len, &peer_iface, &peer_iface_len);

    int is_reachable = uct_iface_is_reachable(iface, peer_dev, peer_iface);
    if (is_reachable == 0) {
        fprintf(stderr, "uct_iface_is_reachable returned false. Peer is not reachable for this iface\n");
        exit(EXIT_FAILURE);
    }
    printf("uct_iface_is_reachable returned true\n");

    // UCT_IFACE_FLAG_CONNECT_TO_IFACE case seems simpler than UCT_IFACE_FLAG_CONNECT_TO_IFACE
    // We can either send our endpoint using sockets or we can connect our enpoint to the peer iface
    // Since we have already exchanged peer_iface it seems more sensible to use it to set up the endpoint
    // rather than sending even more data using sockets
    uct_ep_params_t ep_params;
    // Unclear if this is necessary when we use UCT_IFACE_FLAG_CONNECT_TO_IFACE
    // Aha! It is necessary!
    ep_params.field_mask = UCT_EP_PARAM_FIELD_IFACE;
    ep_params.iface = iface;

    // These parameters are definitely specific for UCT_IFACE_FLAG_CONNECT_TO_IFACE
    ep_params.field_mask |= UCT_EP_PARAM_FIELD_DEV_ADDR |
                            UCT_EP_PARAM_FIELD_IFACE_ADDR;
    ep_params.dev_addr    = peer_dev;
    ep_params.iface_addr  = peer_iface;

    uct_ep_h ep;
    status = uct_ep_create(&ep_params, &ep);
    ERROR_CHECK_UCS_OK("uct_ep_create", status)
    printf("uct_ep_create succeeded\n");

    for (int i = 0; i < 100; i++) {
        uct_worker_progress(worker);
        usleep(100);
        if (callback_var != 0) {
            printf("Message arrived after %d iterations\n", i);
        }
    }

    printf("After wait loop, callback_var == %d\n", callback_var);
    

    close(socket_fd);
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

    status = uct_iface_set_am_handler(iface, AM_ID, hello_world,
                                      FUNC_AM_SHORT, 0);
    ERROR_CHECK_UCS_OK("uct_iface_set_am_handler", status)

    // uct_device_addr_t own_dev;
    // uct_iface_addr_t own_iface;

    int oob_sock = -1;  /* OOB connection socket */

    /* get own device address */
    // uct_iface_get_device_address
    size_t own_dev_len = iface_attr.device_addr_len;
    uct_device_addr_t *own_dev = (uct_device_addr_t*)calloc(1, own_dev_len);
    if (own_dev_len > 0) {
        status = uct_iface_get_device_address(iface, own_dev);
        ERROR_CHECK_UCS_OK("uct_iface_get_device_address", status)
    }
    
    /* get own iface address */
    // uct_iface_get_address
    size_t own_iface_len = iface_attr.iface_addr_len;
    uct_iface_addr_t *own_iface = (uct_iface_addr_t*)calloc(1, own_iface_len);
    if (own_iface_len > 0) {
        status = uct_iface_get_address(iface, own_iface);
        ERROR_CHECK_UCS_OK("uct_iface_get_address", status)
    }

    if (is_server) {
        run_ucx_server(
            worker,
            own_dev,
            own_dev_len,
            own_iface,
            own_iface_len,
            iface);
    } else {
        char *server_name = argv[1];
        run_ucx_client(server_name,
            worker,
            own_dev,
            own_dev_len,
            own_iface,
            own_iface_len,
            iface);
    }

    /* Cleanup */
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
    return 0;
}
