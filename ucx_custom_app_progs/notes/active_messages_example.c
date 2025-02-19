
void init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker) {
    ucp_worker_params_t worker_params;
    memset(&worker_params, 0, sizeof(worker_params));

    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    ucp_worker_create(ucp_context, &worker_params, ucp_worker);
}

int message_complete = 0;
bool message_is_rendezvous = false;
void * message_descriptor = NULL;


ucs_status_t ucp_am_data_cb(void *arg, const void *header, size_t header_length,
                            void *data, size_t length,
                            const ucp_am_recv_param_t *param)
{
    // Should do some more stuff I believe. At least set a completed flag
    printf("hello\n");

    message_complete++;

    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) {
        message_is_rendezvous = true;
        message_descriptor = data;
        return UCS_INPROGRESS;
    }

    message_is_rendezvous = false;

    return UCS_OK;
}


void run_server(ucp_context_h ucp_context, ucp_worker_h ucp_worker,
                      char *listen_addr, send_recv_type_t send_recv_type)
{
    ucx_server_ctx_t context;
    ucp_worker_h     ucp_data_worker;
    init_worker(ucp_context, &ucp_data_worker)

    // register am recv callback
    ucp_am_handler_param_t am_handler_param;
    am_handler_param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                       UCP_AM_HANDLER_PARAM_FIELD_CB |
                       UCP_AM_HANDLER_PARAM_FIELD_ARG;
    am_handler_param.id         = TEST_AM_ID;
    am_handler_param.cb         = ucp_am_data_cb;
    am_handler_param.arg        = ucp_worker; /* not used in our callback */
    ucp_worker_set_am_recv_handler(ucp_worker, &am_handler_param);

    // server creates endpoint to client... Why???
    ucp_ep_params_t ep_params;
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_CONN_REQUEST;
    ep_params.conn_request    = conn_request;
    ep_params.err_handler.cb  = err_cb;
    ep_params.err_handler.arg = NULL;

    ucp_ep_create(data_worker, &ep_params, server_ep);

    while (message_complete == 0) {
        ucp_worker_progress(ucp_worker);
    }

    ucp_request_param_t params = {0};
    size_t msg_length;
    void *msg;
    ucs_status_ptr_t request;

    if (message_is_rendezvous) {
        params.op_attr_mask |= UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
        params.cb.recv_am    = am_recv_cb;
        request              = ucp_am_recv_data_nbx(ucp_worker,
                                                    am_data_desc.desc,
                                                    msg, msg_length,
                                                    &params);
    } else {
        request = NULL;
    }

}

bool send_complete = false;

static void send_cb(void *request, ucs_status_t status, void *user_data)
{
    send_complete = true;
}

void run_client(ucp_worker_h ucp_worker, char *server_addr,
                      send_recv_type_t send_recv_type)
{
    ucp_ep_params_t ep_params;

    ep_params.field_mask       = UCP_EP_PARAM_FIELD_FLAGS       |
                                 UCP_EP_PARAM_FIELD_SOCK_ADDR   |
                                 UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                 UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode         = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb   = err_cb;
    ep_params.err_handler.arg  = NULL;
    ep_params.flags            = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    ep_params.sockaddr.addr    = (struct sockaddr*)&connect_addr;
    ep_params.sockaddr.addrlen = sizeof(connect_addr);

    ucp_ep_create(ucp_worker, &ep_params, client_ep);

    ucp_dt_iov_t *iov = alloca(iov_cnt * sizeof(ucp_dt_iov_t));
    ucp_request_param_t params;

    /* Client sends a message to the server using the AM API */
    params.cb.send = (ucp_send_nbx_callback_t)send_cb;
    request        = ucp_am_send_nbx(ep, TEST_AM_ID, NULL, 0ul, msg,
                                     msg_length, &params);

    if (request == NULL) {
        return UCS_OK;
    }

    if (UCS_PTR_IS_ERR(request)) {
        return UCS_PTR_STATUS(request);
    }

    while (!send_complete) {
        ucp_worker_progress(ucp_worker);
    }
    ucs_status_t status = ucp_request_check_status(request);

}


void init_context(ucp_context_h *ucp_context, ucp_worker_h *ucp_worker)
{
    ucp_params_t ucp_params;
    memset(&ucp_params, 0, sizeof(ucp_params));

    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_NAME;
    ucp_params.name       = "client_server";

    ucp_params.features = UCP_FEATURE_AM;

    // need to put config in here though!
    // Don't know why the example doesn't
    ucp_init(&ucp_params, NULL, ucp_context);

    init_worker(*ucp_context, ucp_worker);
}

int main()
{
    ucp_context_h ucp_context;
    ucp_worker_h  ucp_worker;

    char *server_addr = NULL;
    char *listen_addr = NULL;

    init_context(&ucp_context, &ucp_worker);
    
    if (is_server) {
        /* Server side */
        run_server(ucp_context, ucp_worker, listen_addr);
    } else {
        /* Client side */
        run_client(ucp_worker, server_addr, send_recv_type);
    }

}