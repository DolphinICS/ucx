
#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>

#include "stdio.h"

#include "pcie_md.h"
#include "pcie_iface.h"
#include "pcie_ep.h"
#include "pcie_sisci_helper.h"

/* Forward declarations */
static uct_iface_ops_t uct_pcie_iface_ops;

static ucs_config_field_t uct_pcie_iface_config_table[] = {
    {"", "MAX_NUM_EPS=16", NULL,
     ucs_offsetof(uct_pcie_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"SEND_SIZE", "512k",
     "Size of copy-out buffer",
     ucs_offsetof(uct_pcie_iface_config_t, packet_size_bytes),
        UCS_CONFIG_TYPE_MEMUNITS},

    {
        "MAX_EPS", "24", "Max EPs for SCI tl",
        ucs_offsetof(uct_pcie_iface_config_t, max_eps),
        UCS_CONFIG_TYPE_UINT
    },

    {
        "PACKET_QUEUE_LEN", "5", "Message Queue size for each connection",
        ucs_offsetof(uct_pcie_iface_config_t, packet_queue_len),
        UCS_CONFIG_TYPE_UINT
    },

    {NULL}
};

/**
 * @brief Reserve a uct_pcie control descriptor from the uct_pcie_iface's list
 *        of control descriptor.
 * 
 * @param[in] iface 
 * @param[out] cd_index 
 * 
 * @return 
 */
static int uct_pcie_reserve_control_descriptor(
    uct_pcie_iface_t* iface,
    unsigned int *cd_index)
{
    unsigned int i;
    int rc = 0;

    pthread_mutex_lock(&iface->lock);

    /* Find free sci_cd in iface sci_cd list */
    for (i = 0; i < iface->max_eps; i++)
    {
        if(iface->sci_cds[i].cd_status == UCT_PCIE_CD_AVAILABLE) {
            break;
        }
    }
    
    if (i < iface->max_eps) {
        /* Success: reserve sci_cd */
        *cd_index = i;
        iface->sci_cds[i].cd_status = UCT_PCIE_CD_RESERVED;
        iface->connections++;
    } else {
        rc = -1;
    }

    pthread_mutex_unlock(&iface->lock);

    return rc;
}

/**
 * @brief Unreserve a uct_pcie control descriptor from the uct_pcie_iface's list
 *        of control descriptor.
 * 
 * @details helper function to uct_pcie_conn_handler.
 * 
 * @param[inout] iface 
 * @param[in] cd_index 
 */
static void uct_pcie_ureserve_control_descriptor(
    uct_pcie_iface_t* iface,
    unsigned int cd_index)
{
    pthread_mutex_lock(&iface->lock);

    iface->sci_cds[cd_index].cd_status = UCT_PCIE_CD_AVAILABLE;
    iface->connections--;

    pthread_mutex_unlock(&iface->lock);
}

/**
 * @brief Send answer to incoming request.
 * 
 * @details helper function to uct_pcie_conn_handler.
 * 
 * @param[in] iface
 * @param[in] request 
 * @return 
 */
static int uct_pcie_send_answer_to_request(
    uct_pcie_iface_t* iface,
    uct_pcie_conn_req_t* request,
    sci_desc_t sci_virtual_device,
    uct_pcie_conn_desc_t *sci_cd)
{
    uct_pcie_conn_ans_t   answer;
    sci_remote_data_interrupt_t ans_interrupt;
    sci_error_t sci_error;

    do {
        SCIConnectDataInterrupt(
            sci_virtual_device,
            &ans_interrupt,
            request->node_id,
            0,
            request->interrupt,
            1000,
            0,
            &sci_error);
    } while (sci_error != SCI_ERR_OK);

    answer.segment_id = iface->segment_id;
    answer.ep_conn_offset     = sci_cd->ep_conn_offset;
    
    SCITriggerDataInterrupt(
        ans_interrupt,
        (void *) &answer,
        sizeof(answer),
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_warn("SCI Trigger Interrupt: %s", SCIGetErrorString(sci_error));
    }

    SCIDisconnectDataInterrupt(ans_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);

    /* todo, do error checking */
    return 0;
}

/**
 * @brief This function handles incoming connection requests and assigns a
 * sci file descriptor to that connection. Then it replies with information
 * back to the connecting request to enable the incoming conneciton to connect
 * and offset correctly into the iface's recv buffer. The iface also connects
 * to the connectors control block, so we can signal it when we are ready to
 * recv data.
 * 
 * @param[in] arg iface 
 * @param[in] interrupt which interrupt was triggered
 * @param[in] data: Information received from the connecting process
 * @param[in] length: length of data
 * @param[in] sci_error: Not used
 * @return sci_callback_action_t: Returns callback_continue  
 */
static sci_callback_action_t uct_pcie_conn_handler(
    void* arg,
    sci_local_data_interrupt_t interrupt,
    void* data,
    unsigned int length,
    sci_error_t sci_error)
{
    uct_pcie_conn_req_t* request = (uct_pcie_conn_req_t*) data;
    uct_pcie_iface_t* iface = (uct_pcie_iface_t*) arg;
    uct_pcie_md_t* md = ucs_derived_of(iface->super.md, uct_pcie_md_t); 
    unsigned int sci_cd_index;
    uct_pcie_conn_desc_t *sci_cd;

    int ret;

    ret = uct_pcie_reserve_control_descriptor(iface, &sci_cd_index);
    if (ret != 0) {
        ucs_error("Number of endpoints exceeds limit %u", iface->max_eps);
        return SCI_CALLBACK_CONTINUE;
    }
    sci_cd = &(iface->sci_cds[sci_cd_index]);
    
    ret = uct_pcie_send_answer_to_request(
        iface,
        request,
        md->sci_virtual_device,
        sci_cd);
    if (ret != 0) {
        ucs_error("Failed to send answer to connection request");
        uct_pcie_ureserve_control_descriptor(iface, sci_cd_index);
        return SCI_CALLBACK_CONTINUE;
    }

    /* Connect to remote endpoint's control buffer */
    ret = uct_pcie_connect_segment(
        iface->vdev_ctl,
        request->ep_conn_index * sizeof(uct_pcie_ctl_t),
        sizeof(uct_pcie_ctl_t),
        request->node_id,
        request->ctl_segment_id,
        &sci_cd->ctl_segment,
        &sci_cd->ctl_segment_map,
        (volatile void **)&sci_cd->ctl_buf);
    if (ret != 0) {
        ucs_error("Failed to connect to remote control buffer");
        uct_pcie_ureserve_control_descriptor(iface, sci_cd_index);
        return SCI_CALLBACK_CONTINUE;
    }
            
    sci_cd->cd_status = UCT_PCIE_CD_READY;
    return SCI_CALLBACK_CONTINUE;
}

static int
uct_pcie_ipc_iface_is_reachable_v2(
    const uct_iface_h tl_iface,
    const uct_iface_is_reachable_params_t *params)
{
    return 1;
}

int uct_pcie_ipc_ep_is_connected(
    const uct_ep_h tl_ep,
    const uct_ep_is_connected_params_t *params)
{
    return 1;
}

static uct_iface_internal_ops_t uct_base_iface_internal_ops = {
    .iface_estimate_perf   = uct_base_iface_estimate_perf,
    .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query              = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_pcie_ipc_iface_is_reachable_v2,
    .ep_is_connected       = uct_pcie_ipc_ep_is_connected
};

/**
 * @brief Construct a new ucs class init func object
 * 
 * @param md 
 * @param worker 
 * @param params 
 * @param tl_config 
 */
static UCS_CLASS_INIT_FUNC(
    uct_pcie_iface_t,
    uct_md_h md,
    uct_worker_h worker,
    const uct_iface_params_t *params,
    const uct_iface_config_t *tl_config)
{
    unsigned int nodeID;
    unsigned int adapterID = 0;
    unsigned int flags = 0;
    int ret;
    ssize_t i = 0;
    sci_error_t sci_error;
    unsigned dma_seg_id;
    sci_cb_data_interrupt_t callback = uct_pcie_conn_handler;
    uct_pcie_iface_config_t* config = ucs_derived_of(tl_config, uct_pcie_iface_config_t); 
    uct_pcie_md_t * sci_md = ucs_derived_of(md, uct_pcie_md_t);

    size_t packet_queue_size_bytes;
    size_t recv_segment_size;
    size_t control_segment_size;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");
    
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }

    if (ucs_derived_of(worker, uct_priv_worker_t)->thread_mode == UCS_THREAD_MODE_MULTI) {
        ucs_error("SCI transport does not support multi-threaded worker");
        return UCS_ERR_INVALID_PARAM;
    }

    if (pthread_mutex_init(&self->lock, NULL) != 0) {
        printf("\n mutex init failed\n");
        goto err;
    }

    /* Arbiter for ep_pending_add/ep_pending_purge. Must be initialised before
     * any EP can be created, since EPs schedule their groups here. */
    ucs_arbiter_init(&self->arbiter);

    UCS_CLASS_CALL_SUPER_INIT(
            uct_base_iface_t, &uct_pcie_iface_ops, &uct_base_iface_internal_ops,
            md, worker, params,
            tl_config UCS_STATS_ARG(
                    (params->field_mask & UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                            params->stats_root :
                            NULL) UCS_STATS_ARG(UCT_PCIE_NAME));
    


    //---------- IFACE sci --------------------------
    SCIGetLocalNodeId(adapterID, &nodeID, flags, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCI_IFACE_INIT: %s", SCIGetErrorString(sci_error));
        goto err_destroy_mutex;
    }

    /* uct_pcie_iface_t *self */
    self->device_addr = nodeID;
    self->packet_size_bytes = config->packet_size_bytes;
    self->eps_init_cnt = 0;
    self->max_eps = MIN(UCT_PCIE_MAX_EPS, config->max_eps);
    self->connections = 0;
    self->packet_queue_len  = config->packet_queue_len;

    SCIOpen(&self->vdev_ep, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCIOpen: %s", SCIGetErrorString(sci_error));
        goto err_destroy_mutex;
    }

    SCIOpen(&self->vdev_ctl, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCIOpen: %s", SCIGetErrorString(sci_error));
        goto err_free_vdev_ep;
    }     

    packet_queue_size_bytes = self->packet_size_bytes * self->packet_queue_len;

    /*  recv segment    */
    recv_segment_size = self->max_eps * packet_queue_size_bytes;
    ret = uct_pcie_helper_create_seg_set_avail(
        sci_md->sci_virtual_device,
        &self->local_segment,
        &self->local_map,
        recv_segment_size,
        &self->segment_id,
        (void**)&self->recv_buffer);
    if (ret != 0) {
        ucs_error("Failed to set up receive segment");
        goto err_free_vdev_ctl;
    }

    /* ctl segment */
    control_segment_size = sizeof(uct_pcie_ctl_t) * self->max_eps;
    ret = uct_pcie_helper_create_seg_set_avail(
        sci_md->sci_virtual_device,
        &self->ctl_segment,
        &self->ctl_segment_map,
        control_segment_size,
        &self->ctl_segment_id,
        (void**)&self->ctls);
    if (ret != 0) {
        ucs_error("Failed to set up receive segment");
        goto err_remove_recv_seg;
    }

    for(i = 0; i < self->max_eps; i++) {
        self->sci_cds[i].cd_status = UCT_PCIE_CD_AVAILABLE;
        self->sci_cds[i].ep_conn_offset = i * packet_queue_size_bytes; 
        self->sci_cds[i].packet_queue_buf =
            &self->recv_buffer[self->sci_cds[i].ep_conn_offset];
        self->sci_cds[i].ep_conn_last_ack = 0;
        self->ctls[i].ep_conn_ack = 0;
    }

    /* --- Initialize DMA related resources --- */
    ret = uct_pcie_helper_create_segment(
        sci_md->sci_virtual_device,
        &self->dma_segment,
        &self->dma_map,
        self->packet_size_bytes,
        &dma_seg_id,
        &self->dma_buffer);
    if (ret != 0) {
        ucs_error("Failed to set up receive segment");
        goto err_remove_ctl_seg;
    }

    SCICreateDMAQueue(
        sci_md->sci_virtual_device,
        &self->dma_queue,
        0,
        10,
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("CreateDMAQueue: %s", SCIGetErrorString(sci_error));
        goto err_remove_dma_seg;
    } 

    /* --- Initialize data interrupts for managing incoming connections --- */
    SCICreateDataInterrupt(
        sci_md->sci_virtual_device,
        &self->interrupt,
        0,
        &self->interrupt_no,  
        callback,
        self,
        SCI_FLAG_USE_CALLBACK,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCICreateDataInterrupt: %s", SCIGetErrorString(sci_error));
        goto err_remove_dma_queue;
    }

    /*Need to find out how mpool works and how it is used by the underlying systems in ucx*/
    /*status = uct_iface_param_am_alignment(params, self->packet_size_bytes, 0, 0,
                                          &alignment, &align_offset);


    if (status != UCS_OK) {
        printf("failed to init sci mpool\n");
        return status;
    }*/

    ucs_debug("iface_addr: %d dev_addr: %d segment_size %zd\n",
        self->interrupt_no,
        self->device_addr,
        self->packet_size_bytes);

    return UCS_OK;

err_remove_dma_queue:
    SCIRemoveDMAQueue(self->dma_queue, UCT_PCIE_NO_FLAGS, &sci_error);
    if(sci_error != SCI_ERR_OK) {
        printf("SCIRemoveDMAQueue failed: %s\n", SCIGetErrorString(sci_error));
    }
err_remove_dma_seg:
    uct_pcie_helper_remove_segment(self->dma_segment, self->dma_map);
err_remove_ctl_seg:
    uct_pcie_helper_remove_seg_set_unavail(self->ctl_segment, self->ctl_segment_map);
err_remove_recv_seg:
    uct_pcie_helper_remove_seg_set_unavail(self->local_segment, self->local_map);
err_free_vdev_ctl:
    SCIClose(self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
err_free_vdev_ep:
    SCIClose(self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
err_destroy_mutex:
    pthread_mutex_destroy(&self->lock);
err:
    return UCS_ERR_NO_RESOURCE;
}


/**
 * @brief Construct a new ucs class cleanup func object
 * 
 */

static UCS_CLASS_CLEANUP_FUNC(uct_pcie_iface_t)
{
    sci_error_t sci_error;
    
    /* Cleanup for uct_pcie_iface_progress_enable */
    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND |
                                    UCT_PROGRESS_RECV);
    
    /* Remove data interrupt used for connection handling. This was set up in
     * init, but it makes sense to clean it up before disconnecting from
     * segments. Since removing this data interrupt effectively stops any new
     * connections from being set up. */
    SCIRemoveDataInterrupt(self->interrupt, UCT_PCIE_NO_FLAGS, &sci_error);

    /* Remove connections set up by connection handler uct_pcie_conn_handler for
     * any connections initiated by a remote endpoint */
    for(ssize_t i = 0; i < self->connections; i++) {
        if (self->sci_cds[i].cd_status == UCT_PCIE_CD_READY) {
            self->sci_cds[i].cd_status = UCT_PCIE_CD_RESERVED;
            uct_pcie_disconnect_segment(
                self->sci_cds[i].ctl_segment,
                self->sci_cds[i].ctl_segment_map);
            self->sci_cds[i].cd_status = UCT_PCIE_CD_AVAILABLE;
        }
    }
    
    /* ----  Remove other resources set up on init --- */

    /* Clean up DMA related resources */
    SCIRemoveDMAQueue(self->dma_queue, UCT_PCIE_NO_FLAGS, &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCIRemoveDMAQueue: %s", SCIGetErrorString(sci_error));
    }
    uct_pcie_helper_remove_segment(self->dma_segment, self->dma_map);


    /* Remove data segment where iface receives packages from endpoint */
    uct_pcie_helper_remove_seg_set_unavail(
        self->local_segment,
        self->local_map);

    /* Remove control segment where iface sends acknowledgements */
    uct_pcie_helper_remove_seg_set_unavail(
        self->ctl_segment,
        self->ctl_segment_map);

    /* Closing device descriptors used for connections */
    SCIClose(self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);

    /* All EPs must have been destroyed (and their pending queues purged) before
     * the iface is cleaned up, so the arbiter should be empty here. */
    ucs_arbiter_cleanup(&self->arbiter);

    pthread_mutex_destroy(&self->lock);
}


/* block of macros defining the interface class */ 
UCS_CLASS_DEFINE(uct_pcie_iface_t, uct_base_iface_t);

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_pcie_iface_t, uct_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(
    uct_pcie_iface_t,
    uct_iface_t,
    uct_md_h,
    uct_worker_h,
    const uct_iface_params_t*,
    const uct_iface_config_t*);


static ucs_status_t uct_pcie_query_devices(
    uct_md_h md,
    uct_tl_device_resource_t **devices_p,
    unsigned *num_devices_p)
{
    ucs_status_t status = -1;
       
    status = uct_single_device_resource(
        md,
        UCT_PCIE_NAME,
        UCT_DEVICE_TYPE_NET,
        UCS_SYS_DEVICE_ID_UNKNOWN,
        devices_p,
        num_devices_p);
    
    return status; 
}

static int uct_pcie_iface_is_reachable(
    const uct_iface_h tl_iface,
    const uct_device_addr_t *dev_addr,
    const uct_iface_addr_t *iface_addr)
{
    return 1;
}

ucs_status_t uct_pcie_get_device_address(
    uct_iface_h iface,
    uct_device_addr_t *addr)
{
    uct_pcie_iface_t* sci_iface = ucs_derived_of(iface, uct_pcie_iface_t);
    uct_pcie_device_addr_t* sci_addr = (uct_pcie_device_addr_t *) addr;
    
    sci_addr->node_id = sci_iface->device_addr;
    
    return UCS_OK;
}


/**
 * @brief returns the ID used for the connection interrupt
 *  
 */
ucs_status_t uct_pcie_iface_get_address(
    uct_iface_h tl_iface,
    uct_iface_addr_t *addr)
{
    uct_pcie_iface_t *iface    = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_md_t    *md       = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    uct_pcie_iface_addr_t *iface_addr = (uct_pcie_iface_addr_t *)addr;

    iface_addr->interrupt_no    = iface->interrupt_no;
    iface_addr->rma_seg_id = md->rma_seg_id;
    iface_addr->rma_base_va    = (uint64_t)(uintptr_t)md->rma_buf;

    return UCS_OK;
}


void uct_pcie_iface_progress_enable(uct_iface_h iface, unsigned flags) {
    uct_base_iface_progress_enable(iface, flags);
}

/**
 * @brief Arbiter dispatch callback for pending send requests.
 *
 * Called by ucs_arbiter_dispatch once per pending element (quota=1 per group
 * per progress tick). We call the request's own retry function. If the
 * flow-control window is still full the group is rescheduled; otherwise the
 * element is removed and UCX is notified that the send completed.
 */
static ucs_arbiter_cb_result_t
uct_pcie_ep_dispatch_pending(
    ucs_arbiter_t       *arbiter,
    ucs_arbiter_group_t *group,
    ucs_arbiter_elem_t  *elem,
    void                *arg)
{
    uct_pending_req_t *req    = ucs_container_of(elem, uct_pending_req_t, priv);
    ucs_status_t       status = req->func(req);

    if (status == UCS_OK) {
        /* Send succeeded: remove this element and let the arbiter move on. */
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        /* Shouldn't normally happen for our synchronous sends, but handled
         * for correctness: the request is partially in flight, skip the group
         * for this round. */
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    } else {
        /* UCS_ERR_NO_RESOURCE (window still full) or any other transient
         * error: reschedule the group so it is retried on the next progress
         * tick rather than spinning here. */
        return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
    }
}

/**
 * @brief Process all pending incoming packets across all connections.
 *
 * For each ready connection, drains all consecutively posted packets
 * from the ring buffer slot by slot. For each packet, the AM handler
 * is invoked and the ack counter is advanced so the remote sender
 * can reuse the slot.
 *
 * Note: Only processes packets in order — stops at the first unposted
 * slot, since the sender guarantees sequential posting.
 *
 * @return Number of packets processed
 */
unsigned uct_pcie_iface_progress(uct_iface_h tl_iface) {
    uct_pcie_iface_t     *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_conn_desc_t *cd;
    uct_pcie_am_hdr_t    *packet;
    void                 *packet_payload_ptr;
    ucs_status_t          ucs_ret;
    uint32_t              packet_queue_index;
    uint32_t              packet_offset;
    unsigned              count = 0;

    for (size_t i = 0; i < iface->connections; i++) {
        cd = &iface->sci_cds[i];

        if (cd->cd_status != UCT_PCIE_CD_READY) {
            continue;
        }

        /* Drain all consecutively posted packets for this connection */
        while (1) {
            packet_queue_index = (cd->ep_conn_last_ack + 1) % iface->packet_queue_len;
            packet_offset      = iface->packet_size_bytes * packet_queue_index;
            packet             = (uct_pcie_am_hdr_t *) &cd->packet_queue_buf[packet_offset];

            /* Stop if the next slot has not been posted by the sender yet */
            if (packet->am_message_posted != 1) {
                break;
            }

            packet_payload_ptr = (void *)
                &cd->packet_queue_buf[packet_offset + sizeof(uct_pcie_am_hdr_t)];

            ucs_ret = uct_iface_invoke_am(
                &iface->super,
                packet->am_id,
                packet_payload_ptr,
                packet->am_length,
                0);

            if (ucs_ret == UCS_INPROGRESS) {
                /* AM handler is not done with the buffer yet, retry next progress call */
                ucs_debug("uct_pcie_iface_progress in progress");
                break;
            }

            if (ucs_ret != UCS_OK) {
                ucs_error("uct_pcie_iface_progress returned error %d", ucs_ret);
                break;
            }

            /* Mark the slot as free and notify the sender by advancing the ack counter */
            packet->am_message_posted = 0;
            cd->ep_conn_last_ack++;
            cd->ctl_buf->ep_conn_ack = cd->ep_conn_last_ack;
            SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

            count++;
        }
    }

    /* After draining incoming packets, ack counters have advanced, which may
     * have opened up flow-control slots. Dispatch any pending send requests
     * that were blocked. Quota=1 means at most one element per EP per tick,
     * preventing any single EP from starving others. */
    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_pcie_ep_dispatch_pending, NULL);

    return count;
}

static ucs_status_t uct_pcie_iface_query(
    uct_iface_h tl_iface,
    uct_iface_attr_t *attr)
{
    size_t am_max;
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);

    uct_base_iface_query(ucs_derived_of(tl_iface, uct_base_iface_t), attr);

    attr->cap.flags = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                      UCT_IFACE_FLAG_CB_SYNC          |
                      UCT_IFACE_FLAG_AM_SHORT         |
                      UCT_IFACE_FLAG_AM_BCOPY         |
                      UCT_IFACE_FLAG_AM_ZCOPY         |
                      UCT_IFACE_FLAG_PUT_SHORT        |
                      UCT_IFACE_FLAG_PUT_BCOPY        |
                      UCT_IFACE_FLAG_GET_SHORT        |
                      UCT_IFACE_FLAG_GET_BCOPY;
    attr->cap.event_flags = 0;

    attr->device_addr_len = sizeof(uct_pcie_device_addr_t);
    attr->iface_addr_len  = sizeof(uct_pcie_iface_addr_t);
    attr->ep_addr_len     = 0;

    /* AM limits: each slot holds one uct_pcie_am_hdr_t followed by user data.
     * All variants share the same slot-remainder bound. */
    am_max = iface->packet_size_bytes - sizeof(uct_pcie_am_hdr_t);
    attr->cap.am.max_short = am_max;
    attr->cap.am.max_bcopy = am_max;
    attr->cap.am.min_zcopy = 0;
    attr->cap.am.max_zcopy = am_max;
    attr->cap.am.max_iov   = 10;
    attr->cap.am.max_hdr   = am_max;

    /* Put limits: writes go directly into the remote SISCI segment through the
     * PCIe MMIO window.  put_bcopy has no transport-level size limit (bounded
     * only by the remote segment size).  put_short is limited to keep it a
     * fast inline path. */
    attr->cap.put.max_short        = UCT_PCIE_MAX_PUT_SHORT;
    attr->cap.put.max_bcopy        = ULONG_MAX;
    attr->cap.put.min_zcopy        = 0;
    attr->cap.put.max_zcopy        = 0; /* not implemented */
    attr->cap.put.opt_zcopy_align  = 1;
    attr->cap.put.align_mtu        = 1;
    attr->cap.put.max_iov          = 1;

    /* Get limits: reads go directly from the remote SISCI segment window.
     * Synchronous, no zcopy variant. */
    attr->cap.get.max_short        = UCT_PCIE_MAX_PUT_SHORT;
    attr->cap.get.max_bcopy        = ULONG_MAX;
    attr->cap.get.min_zcopy        = 0;
    attr->cap.get.max_zcopy        = 0;
    attr->cap.get.opt_zcopy_align  = 1;
    attr->cap.get.align_mtu        = 1;
    attr->cap.get.max_iov          = 1;

    /* Conservative PCIe performance estimates.
     * Latency: ~1 microsecond for a PCIe round trip.
     * Bandwidth: ~20 GB/s (conservative for PCIe 4.0 x16, actual numbers depends on slot width and gen).
     */
    attr->latency             = ucs_linear_func_make(1e-6, 0);
    attr->bandwidth.dedicated = 20 * UCS_GBYTE;
    attr->bandwidth.shared    = 0;
    attr->overhead            = 1e-6;
    attr->priority            = 0;

    return UCS_OK;
}

static uct_iface_ops_t uct_pcie_iface_ops = {
    .ep_put_short             = uct_pcie_ep_put_short,
    .ep_put_bcopy             = uct_pcie_ep_put_bcopy,
    .ep_get_short             = uct_pcie_ep_get_short,
    .ep_get_bcopy             = uct_pcie_ep_get_bcopy,
    
    .ep_am_short              = uct_pcie_ep_am_short,
    .ep_am_short_iov          = uct_pcie_ep_am_short_iov,
    .ep_am_bcopy              = uct_pcie_ep_am_bcopy,
    .ep_am_zcopy              = uct_pcie_ep_am_zcopy,
    
    .ep_atomic_cswap64        = uct_pcie_ep_atomic_cswap64, /* Stubbed */
    .ep_atomic64_post         = uct_pcie_ep_atomic64_post, /* Stubbed */
    .ep_atomic64_fetch        = uct_pcie_ep_atomic64_fetch, /* Stubbed */
    .ep_atomic_cswap32        = uct_pcie_ep_atomic_cswap32, /* Stubbed */
    .ep_atomic32_post         = uct_pcie_ep_atomic32_post, /* Stubbed */
    .ep_atomic32_fetch        = uct_pcie_ep_atomic32_fetch, /* Stubbed */

    .ep_flush                 = uct_base_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_check                 = ucs_empty_function_return_success,
    .ep_pending_add           = uct_pcie_ep_pending_add,
    .ep_pending_purge         = uct_pcie_ep_pending_purge,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_pcie_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_pcie_ep_t),
    .iface_flush              = uct_base_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = uct_pcie_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_pcie_iface_progress,
    .iface_event_arm          = ucs_empty_function_return_success,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_pcie_iface_t),
    .iface_query              = uct_pcie_iface_query,
    .iface_get_device_address = uct_pcie_get_device_address,
    .iface_get_address        = uct_pcie_iface_get_address,
    .iface_is_reachable       = uct_pcie_iface_is_reachable
};

extern uct_component_t uct_pcie_component;

#define UCT_PCIE_CONFIG_PREFIX "PCIE_"

/**
 * @brief Construct a new uct tl define object
 *  component:
 *  transport name
 *  device_query()
 *  iface type
 *  config prefix
 *  config table
 *  type of config table
 */
UCT_TL_DEFINE(
    &uct_pcie_component,
    pcie,
    uct_pcie_query_devices,
    uct_pcie_iface_t,
    UCT_PCIE_CONFIG_PREFIX,
    uct_pcie_iface_config_table,
    uct_pcie_iface_config_t);


UCS_STATIC_INIT
{
    sci_error_t sci_error;
    SCIInitialize(0,&sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIInitialize error: %s", SCIGetErrorString(sci_error));
    }
}

UCS_STATIC_CLEANUP
{
    SCITerminate();
}
