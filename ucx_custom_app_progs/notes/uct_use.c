

// md
// iface
// iface_attr
// worker

/**
 * @brief 
 * @param[in] dev_name 
 * @param[in] tl_name 
 * @param[in] md
 * @param[in] worker
 * @param[out] iface_attr 
 * @param[out] iface 
 * @return 
 */
static ucs_status_t init_iface(char *dev_name, char *tl_name,
                               uct_worker_h worker,
                               uct_md_h md,
                               uct_iface_attr_t *iface_attr,
                               uct_iface_h *iface)
{
    ucs_status_t        status;
    uct_iface_config_t  *config; /* Defines interface configuration options */
    uct_iface_params_t  params;

    params.field_mask           = UCT_IFACE_PARAM_FIELD_OPEN_MODE   |
                                  UCT_IFACE_PARAM_FIELD_DEVICE      |
                                  UCT_IFACE_PARAM_FIELD_STATS_ROOT  |
                                  UCT_IFACE_PARAM_FIELD_RX_HEADROOM |
                                  UCT_IFACE_PARAM_FIELD_CPU_MASK;
    params.open_mode            = UCT_IFACE_OPEN_MODE_DEVICE;
    params.mode.device.tl_name  = tl_name;
    params.mode.device.dev_name = dev_name;
    params.stats_root           = NULL;
    params.rx_headroom          = sizeof(recv_desc_t);
    UCS_CPU_ZERO(&params.cpu_mask);

    /* Read transport-specific interface configuration */
    status = uct_md_iface_config_read(md, tl_name, NULL, NULL, &config);
    /* Open communication interface */
    status = uct_iface_open(md, worker, &params, config,
                            iface);
    uct_config_release(config);

    /* Enable progress on the interface */
    uct_iface_progress_enable(iface_p->iface,
                              UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    /* Get interface attributes */
    status = uct_iface_query(iface_p->iface, &iface_p->iface_attr);

    // Can check if current device and transport supports active messages:
    // iface_p->iface_attr.cap.flags & UCT_IFACE_FLAG_AM_SHORT
    // iface_p->iface_attr.cap.flags & UCT_IFACE_FLAG_AM_BCOPY
    // iface_p->iface_attr.cap.flags & UCT_IFACE_FLAG_AM_ZCOPY

}


// uct_iface_attr_t    iface_attr; // out
// uct_iface_h         iface; // out
// uct_md_h            md; // out
// uct_md_attr_t       md_attr; // out
// uct_worker_h        worker; // in

/**
 * @brief Looks up device/transport from transport layer (???)
 * @param[in] dev_name 
 * @param[in] tl_name
 *  
 * @param[in] worker 
 * 
 * @param[out] iface_attr 
 * @param[out] iface 
 * 
 * @param[out] md_attr 
 * @param[out] md 
 * 
 * @return 
 */
static ucs_status_t dev_tl_lookup(
    // Specifying transport (often command line arguments)
    char *dev_name, char *tl_name,
    // created before the call to this function, input parameter.
    uct_worker_h worker,
    // These are output parameters
    uct_iface_attr_t *iface_attr,
    uct_iface_h *iface,
    uct_md_attr_t *md_attr,
    uct_md_h *md)
{
    ucs_status_t status;
    uct_component_h *components;
    unsigned num_components;

    status = uct_query_components(&components, &num_components);

    /* Iterate through components (when testing a transport perhaps one does not need to iterate through all of it?) */
    for (int cmpt_index = 0; cmpt_index < num_components; cmpt_index++) {
        uct_component_attr_t component_attr = {0};

        component_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCE_COUNT;
        status = uct_component_query(components[cmpt_index], &component_attr);

        component_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCES;
        component_attr.md_resources = alloca(sizeof(*component_attr.md_resources) *
                                             component_attr.md_resource_count);
        status = uct_component_query(components[cmpt_index], &component_attr);

        /* Iterate through memory domain resources */
        for (int md_index = 0; md_index < component_attr.md_resource_count; ++md_index) {
            uct_md_config_t        *md_config;
            status = uct_md_config_read(components[cmpt_index], NULL, NULL,
                                        &md_config);

            status = uct_md_open(components[cmpt_index],
                                 component_attr.md_resources[md_index].md_name,
                                 md_config, md);

            uct_config_release(md_config);

            status = uct_md_query(*md, md_attr);

            uct_tl_resource_desc_t *tl_resources    = NULL; /* Communication resource descriptor */
            unsigned int num_tl_resources;
            status = uct_md_query_tl_resources(*md, &tl_resources,
                                               &num_tl_resources);

            /* Go through each available transport and find the proper name */
            for (int tl_index = 0; tl_index < num_tl_resources; ++tl_index) {
                uct_tl_resource_desc_t *tl_res = &tl_resources[tl_index];
                if ((strcmp(dev_name, tl_res->dev_name) == 0) && (strcmp(tl_name, tl_res->tl_name) == 0)) {
                    status = init_iface(tl_resources[tl_index].dev_name,
                                        tl_resources[tl_index].tl_name,
                                        worker,
                                        iface_attr,
                                        iface,
                                        *md);
                    if (status != UCS_OK) {
                        break;
                    }
                }
            }
        }
    }

}

static void* desc_holder = NULL;

/* Callback to handle receive active message */
static ucs_status_t hello_world(void *arg, void *data, size_t length,
                                unsigned flags)
{
    func_am_t func_am_type = *(func_am_t *)arg;
    recv_desc_t *rdesc;

    print_strings("callback", func_am_t_str(func_am_type), data, length);

    if (flags & UCT_CB_PARAM_FLAG_DESC) {
        rdesc = (recv_desc_t *)data - 1;
        /* Hold descriptor to release later and return UCS_INPROGRESS */
        rdesc->is_uct_desc = 1;
        desc_holder = rdesc;
        return UCS_INPROGRESS;
    }

    /* We need to copy-out data and return UCS_OK if want to use the data
     * outside the callback */
    rdesc = malloc(sizeof(*rdesc) + length);
    CHKERR_ACTION(rdesc == NULL, "allocate memory\n", return UCS_ERR_NO_MEMORY);
    rdesc->is_uct_desc = 0;
    memcpy(rdesc + 1, data, length);
    desc_holder = rdesc;
    return UCS_OK;
}

void main() {

    ucs_status_t status;

    /* Initialize one context. Best with 1 context per worker */
    ucs_async_context_t *async_context;
    status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async_context);

    /* Create a worker object */
    uct_worker_h worker;
    status = uct_worker_create(async_context, UCS_THREAD_MODE_SINGLE, &worker);

    /* Search for the desired transport */
    char *dev_name = "dev_name";
    char *tl_name = "transport_name";
    uct_iface_attr_t iface_attr;
    uct_iface_h iface;
    uct_md_attr_t md_attr;
    uct_md_h md;
    status = dev_tl_lookup(dev_name, tl_name, worker, &iface_attr, &iface, &md_attr, &md);

    /* Set active message handler */
    uint8_t id = 0;
    status = uct_iface_set_am_handler(iface, id, hello_world,
                                      &cmd_args.func_am_type, 0);
    
    uct_device_addr_t   *own_dev;
    uct_iface_addr_t    *own_iface;
    own_dev = (uct_device_addr_t*)calloc(1, iface_attr.device_addr_len);

    oob_sock = connect_common(cmd_args.server_name, cmd_args.server_port,
                              cmd_args.ai_family);

    status = uct_iface_get_device_address(iface, own_dev);

    status = (ucs_status_t)uct_iface_is_reachable(iface, peer_dev, peer_iface);

    uct_ep_params_t     ep_params;
    ep_params.field_mask = UCT_EP_PARAM_FIELD_IFACE;
    ep_params.iface      = iface;
    uct_ep_h            ep;

    if (if_info.iface_attr.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP) {
        uct_ep_addr_t       *own_ep;
        own_ep = (uct_ep_addr_t*)calloc(1, iface_attr.ep_addr_len);

        /* Create new endpoint */
        status = uct_ep_create(&ep_params, &ep);

        /* Get endpoint address */
        status = uct_ep_get_address(ep, own_ep);

        status = (ucs_status_t)sendrecv(oob_sock, own_ep, if_info.iface_attr.ep_addr_len,
                                        (void **)&peer_ep);

        /* Connect endpoint to a remote endpoint */
        status = uct_ep_connect_to_ep(ep, peer_dev, peer_ep);
        if (barrier(oob_sock, progress_worker, if_info.worker)) {
            status = UCS_ERR_IO_ERROR;
            goto out_free_ep;
        }

    } else if (if_info.iface_attr.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE) {
        /* Create an endpoint which is connected to a remote interface */
        ep_params.field_mask |= UCT_EP_PARAM_FIELD_DEV_ADDR |
                                UCT_EP_PARAM_FIELD_IFACE_ADDR;
        ep_params.dev_addr    = peer_dev;
        ep_params.iface_addr  = peer_iface;
        status = uct_ep_create(&ep_params, &ep);
    }

    bool is_server;
    if (is_server) {
        do {
        /* Send active message to remote endpoint */
        status = uct_ep_am_short(ep, id, send_args.header, send_args.payload,
                                 send_args.len);
        uct_worker_progress(if_info->worker);
        } while (status == UCS_ERR_NO_RESOURCE);

    } else {
        recv_desc_t *rdesc;

        while (desc_holder == NULL) {
            /* Explicitly progress any outstanding active message requests */
            uct_worker_progress(if_info.worker);
        }

        if (rdesc->is_uct_desc) {
            /* Release descriptor because callback returns UCS_INPROGRESS */
            uct_iface_release_desc(rdesc);
        } else {
            free(rdesc);
        }
    }

}
