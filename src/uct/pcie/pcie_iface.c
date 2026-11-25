/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcie_iface.h"
#include "pcie_ep.h"
#include "pcie_md.h"
#include "pcie_sisci_helper.h"

#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>


static uct_iface_ops_t uct_pcie_iface_ops;

static ucs_config_field_t uct_pcie_iface_config_table[] = {
    /* Keep MAX_NUM_EPS and MAX_EPS in sync with UCT_PCIE_MAX_EPS */
    {"", "MAX_NUM_EPS=32", NULL, ucs_offsetof(uct_pcie_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"SEND_SIZE", "512k",
     "Size of an active message slot, including its header",
     ucs_offsetof(uct_pcie_iface_config_t, packet_size_bytes),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"MAX_EPS", "32", "Maximum number of simultaneous endpoint connections",
     ucs_offsetof(uct_pcie_iface_config_t, max_eps), UCS_CONFIG_TYPE_UINT},

    {"PACKET_QUEUE_LEN", "5", "Message queue size for each connection",
     ucs_offsetof(uct_pcie_iface_config_t, packet_queue_len),
     UCS_CONFIG_TYPE_UINT},

    {NULL}
};

/* Reserve a free connection descriptor. Returns UCS_ERR_EXCEEDS_LIMIT if all
 * max_eps descriptors are taken. */
static ucs_status_t uct_pcie_iface_conn_desc_reserve(uct_pcie_iface_t *iface,
                                                     unsigned int *cd_index)
{
    ucs_status_t status = UCS_OK;
    unsigned int i;

    pthread_mutex_lock(&iface->lock);

    for (i = 0; i < iface->max_eps; i++) {
        if (iface->sci_cds[i].cd_status == UCT_PCIE_CD_AVAILABLE) {
            break;
        }
    }

    if (i < iface->max_eps) {
        *cd_index                   = i;
        iface->sci_cds[i].cd_status = UCT_PCIE_CD_RESERVED;
        iface->connections++;
    } else {
        status = UCS_ERR_EXCEEDS_LIMIT;
    }

    pthread_mutex_unlock(&iface->lock);

    return status;
}

/* Release a descriptor reserved by uct_pcie_iface_conn_desc_reserve */
static void
uct_pcie_iface_conn_desc_release(uct_pcie_iface_t *iface, unsigned int cd_index)
{
    pthread_mutex_lock(&iface->lock);

    iface->sci_cds[cd_index].cd_status = UCT_PCIE_CD_AVAILABLE;
    iface->connections--;

    pthread_mutex_unlock(&iface->lock);
}

/* Handle one connection request: create a receive segment for the peer EP,
 * map the peer EP's ctl slot and answer there */
static void uct_pcie_iface_accept(uct_pcie_iface_t *iface,
                                  const uct_pcie_conn_req_t *request)
{
    unsigned int sci_cd_index;
    uct_pcie_conn_desc_t *sci_cd;
    ucs_status_t status;

    status = uct_pcie_iface_conn_desc_reserve(iface, &sci_cd_index);
    if (status != UCS_OK) {
        ucs_error("number of endpoints exceeds limit %u", iface->max_eps);
        return;
    }

    sci_cd = &(iface->sci_cds[sci_cd_index]);

    /* Create a dedicated receive segment for this connection */
    status = uct_pcie_segment_create_available(
            iface->vdev_recv,
            iface->packet_size_bytes * iface->packet_queue_len,
            &sci_cd->recv_segment, &sci_cd->recv_segment_map,
            &sci_cd->recv_segment_id, (void**)&sci_cd->packet_queue_buf);
    if (status != UCS_OK) {
        ucs_error("failed to create per-connection receive segment");
        goto err_release_cd;
    }

    /* The peer EP's ctl slot takes both the answer and the acks */
    status = uct_pcie_segment_connect(
            iface->vdev_ctl, request->node_id, request->ctl_segment_id,
            request->ep_conn_index * sizeof(uct_pcie_ctl_t),
            sizeof(uct_pcie_ctl_t), NULL, NULL, &sci_cd->ctl_segment,
            &sci_cd->ctl_segment_map, (volatile void**)&sci_cd->ctl_buf);
    if (status != UCS_OK) {
        ucs_error("failed to connect to remote control buffer");
        goto err_remove_recv_seg;
    }

    /* The EP polls conn_answered, so the segment ID has to land first */
    sci_cd->ctl_buf->recv_seg_id = sci_cd->recv_segment_id;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    sci_cd->ctl_buf->conn_answered = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    sci_cd->cd_status = UCT_PCIE_CD_READY;
    return;

err_remove_recv_seg:
    uct_pcie_segment_remove_available(sci_cd->recv_segment,
                                      sci_cd->recv_segment_map);
err_release_cd:
    uct_pcie_iface_conn_desc_release(iface, sci_cd_index);
}

/* SISCI callback for the connection interrupt. Triggers that arrive while one
 * is pending merge into one call, so every inbox slot is checked. */
static sci_callback_action_t
uct_pcie_iface_conn_handler(void *arg, sci_local_interrupt_t interrupt,
                            sci_error_t sci_error)
{
    uct_pcie_iface_t *iface = (uct_pcie_iface_t*)arg;
    uct_pcie_inbox_slot_t *slot;
    uct_pcie_conn_req_t request;
    unsigned int i;

    if (sci_error != SCI_ERR_OK) {
        ucs_debug("iface %p connection interrupt: %s", iface,
                  SCIGetErrorString(sci_error));
        return SCI_CALLBACK_CONTINUE;
    }

    for (i = 0; i < UCT_PCIE_INBOX_SLOTS; i++) {
        slot = &iface->inbox[i];
        if (!slot->pending) {
            continue;
        }

        /* The initiator wrote the request before pending. The slot is free
         * again before the answer goes out, so the initiator's next request
         * finds it free. */
        ucs_memory_cpu_load_fence();
        request       = slot->request;
        slot->pending = 0;
        uct_pcie_iface_accept(iface, &request);
    }

    return SCI_CALLBACK_CONTINUE;
}

static int
uct_pcie_iface_is_reachable_v2(const uct_iface_h tl_iface,
                               const uct_iface_is_reachable_params_t *params)
{
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_md_t *md       = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    const uct_pcie_device_addr_t *dev_addr;
    sci_error_t sci_error;
    int reachable;

    if (!(params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_DEVICE_ADDR)) {
        return 1; /* no address to probe; assume reachable */
    }

    dev_addr  = (const uct_pcie_device_addr_t*)params->device_addr;
    reachable = SCIProbeNode(md->sci_virtual_device, UCT_PCIE_LOCAL_ADAPTER_NO,
                             dev_addr->node_id, UCT_PCIE_NO_FLAGS, &sci_error);

    if (!reachable &&
        (params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING) &&
        (params->field_mask &
         UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING_LENGTH)) {
        ucs_snprintf_zero(params->info_string, params->info_string_length,
                          "SISCI node %u is not reachable via PCIe adapter %u",
                          dev_addr->node_id, UCT_PCIE_LOCAL_ADAPTER_NO);
    }

    return reachable;
}

static int uct_pcie_ep_is_connected(const uct_ep_h tl_ep,
                                    const uct_ep_is_connected_params_t *params)
{
    const uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);

    return ep->is_connected;
}

static uct_iface_internal_ops_t uct_pcie_iface_internal_ops = {
    .iface_estimate_perf = uct_base_iface_estimate_perf,
    .iface_vfs_refresh   = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)
            ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = (uct_ep_connect_to_ep_v2_func_t)
            ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_pcie_iface_is_reachable_v2,
    .ep_is_connected       = uct_pcie_ep_is_connected
};

static UCS_CLASS_INIT_FUNC(uct_pcie_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_pcie_iface_config_t *config = ucs_derived_of(tl_config,
                                                     uct_pcie_iface_config_t);
    uct_pcie_md_t *sci_md           = ucs_derived_of(md, uct_pcie_md_t);
    unsigned int node_id;
    unsigned int i;
    unsigned int dma_seg_id;
    size_t control_segment_size;
    sci_error_t sci_error;
    ucs_status_t status;
    int ret;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");

    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }

    if (ucs_derived_of(worker, uct_priv_worker_t)->thread_mode ==
        UCS_THREAD_MODE_MULTI) {
        ucs_error("pcie transport does not support multi-threaded worker");
        return UCS_ERR_INVALID_PARAM;
    }

    ret = pthread_mutex_init(&self->lock, NULL);
    if (ret != 0) {
        ucs_error("pthread_mutex_init failed: %s", strerror(ret));
        return UCS_ERR_NO_RESOURCE;
    }

    /* Arbiter must be ready before any EP is created, since EPs schedule
     * their pending groups here. */
    ucs_arbiter_init(&self->arbiter);
    ucs_list_head_init(&self->connecting_eps);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_pcie_iface_ops,
                              &uct_pcie_iface_internal_ops, md, worker, params,
                              tl_config UCS_STATS_ARG(
                                      (params->field_mask &
                                       UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                                              params->stats_root :
                                              NULL)
                                      UCS_STATS_ARG(UCT_PCIE_NAME));

    SCIGetLocalNodeId(UCT_PCIE_LOCAL_ADAPTER_NO, &node_id, UCT_PCIE_NO_FLAGS,
                      &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIGetLocalNodeId failed: %s", SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_destroy_mutex;
    }

    /* The inbox has a slot per node, see uct_pcie_inbox_slot_t */
    if ((node_id / 4) >= UCT_PCIE_INBOX_NODES) {
        ucs_error("SISCI node id %u is above %u, the highest the pcie"
                  " transport supports", node_id,
                  (UCT_PCIE_INBOX_NODES - 1) * 4);
        status = UCS_ERR_UNSUPPORTED;
        goto err_destroy_mutex;
    }

    self->device_addr       = node_id;
    self->packet_size_bytes = config->packet_size_bytes;
    self->packet_queue_len  = config->packet_queue_len;
    self->max_eps           = ucs_min(UCT_PCIE_MAX_EPS, config->max_eps);
    self->eps_init_cnt      = 0;
    self->connections       = 0;

    /* One descriptor per segment role, see uct_pcie_iface_t */
    SCIOpen(&self->vdev_recv, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_recv) failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_destroy_mutex;
    }

    SCIOpen(&self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_ep) failed: %s", SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_close_vdev_recv;
    }

    SCIOpen(&self->vdev_rma, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_rma) failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_close_vdev_ep;
    }

    SCIOpen(&self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_ctl) failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_close_vdev_rma;
    }

    SCIOpen(&self->vdev_setup, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_setup) failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_close_vdev_ctl;
    }

    /* Peers write their ack counters into the control segment */
    control_segment_size = sizeof(uct_pcie_ctl_t) * self->max_eps;
    status = uct_pcie_segment_create_available(sci_md->sci_virtual_device,
                                               control_segment_size,
                                               &self->ctl_segment,
                                               &self->ctl_segment_map,
                                               &self->ctl_segment_id,
                                               (void**)&self->ctls);
    if (status != UCS_OK) {
        ucs_error("failed to set up control segment");
        goto err_close_vdev_setup;
    }

    /* Initialize connection descriptors. Receive segments are created per
     * connection in uct_pcie_iface_accept. */
    for (i = 0; i < self->max_eps; i++) {
        self->sci_cds[i].cd_status        = UCT_PCIE_CD_AVAILABLE;
        self->sci_cds[i].ep_conn_last_ack = 0;
        self->ctls[i].ep_conn_ack         = 0;
        self->ctls[i].recv_seg_id         = 0;
        self->ctls[i].conn_answered       = 0;
    }

    /* The first free inbox segment ID gives this iface its index on the node */
    for (i = 0; i < UCT_PCIE_MAX_IFACES_PER_NODE; i++) {
        status = uct_pcie_segment_create_available_id(
                sci_md->sci_virtual_device,
                sizeof(uct_pcie_inbox_slot_t) * UCT_PCIE_INBOX_SLOTS,
                UCT_PCIE_INBOX_SEG_ID_BASE + i, &self->inbox_segment,
                &self->inbox_map, (void**)&self->inbox);
        if (status != UCS_ERR_BUSY) {
            break;
        }
    }

    if (status == UCS_ERR_BUSY) {
        ucs_error("node %u already has %d pcie ifaces open, the most"
                  " supported (UCT_PCIE_MAX_IFACES_PER_NODE)", node_id,
                  UCT_PCIE_MAX_IFACES_PER_NODE);
        status = UCS_ERR_EXCEEDS_LIMIT;
        goto err_remove_ctl_seg;
    } else if (status != UCS_OK) {
        ucs_error("failed to set up inbox segment");
        goto err_remove_ctl_seg;
    }

    self->local_index  = i;
    self->inbox_seg_id = UCT_PCIE_INBOX_SEG_ID_BASE + i;
    for (i = 0; i < UCT_PCIE_INBOX_SLOTS; i++) {
        self->inbox[i].pending = 0;
    }

    /* DMA staging segment for get, large enough for any read */
    status = uct_pcie_segment_create(sci_md->sci_virtual_device,
                                     UCT_PCIE_RMA_SEG_SIZE, &self->dma_segment,
                                     &self->dma_map, &dma_seg_id,
                                     &self->dma_buffer);
    if (status != UCS_OK) {
        ucs_error("failed to set up DMA staging segment");
        goto err_remove_inbox_seg;
    }

    SCICreateDMAQueue(sci_md->sci_virtual_device, &self->dma_queue,
                      UCT_PCIE_LOCAL_ADAPTER_NO, UCT_PCIE_DMA_QUEUE_MAX_ENTRIES,
                      UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCICreateDMAQueue failed: %s", SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_remove_dma_seg;
    }

    /* Peers trigger this after writing a request into the inbox */
    SCICreateInterrupt(sci_md->sci_virtual_device, &self->interrupt,
                       UCT_PCIE_LOCAL_ADAPTER_NO, &self->interrupt_no,
                       uct_pcie_iface_conn_handler, self, SCI_FLAG_USE_CALLBACK,
                       &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCICreateInterrupt failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_remove_dma_queue;
    }

    ucs_debug("iface %p node %u index %u interrupt %u inbox %u packet size %zu",
              self, self->device_addr, self->local_index, self->interrupt_no,
              self->inbox_seg_id, self->packet_size_bytes);

    return UCS_OK;

err_remove_dma_queue:
    SCIRemoveDMAQueue(self->dma_queue, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIRemoveDMAQueue failed: %s", SCIGetErrorString(sci_error));
    }
err_remove_dma_seg:
    uct_pcie_segment_remove(self->dma_segment, self->dma_map);
err_remove_inbox_seg:
    uct_pcie_segment_remove_available(self->inbox_segment, self->inbox_map);
err_remove_ctl_seg:
    uct_pcie_segment_remove_available(self->ctl_segment, self->ctl_segment_map);
err_close_vdev_setup:
    SCIClose(self->vdev_setup, UCT_PCIE_NO_FLAGS, &sci_error);
err_close_vdev_ctl:
    SCIClose(self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
err_close_vdev_rma:
    SCIClose(self->vdev_rma, UCT_PCIE_NO_FLAGS, &sci_error);
err_close_vdev_ep:
    SCIClose(self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
err_close_vdev_recv:
    SCIClose(self->vdev_recv, UCT_PCIE_NO_FLAGS, &sci_error);
err_destroy_mutex:
    pthread_mutex_destroy(&self->lock);
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_pcie_iface_t)
{
    uct_pcie_conn_desc_t *cd;
    sci_error_t sci_error;
    unsigned int i;

    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    /* Stop new connections before tearing down segments */
    SCIRemoveInterrupt(self->interrupt, UCT_PCIE_NO_FLAGS, &sci_error);

    for (i = 0; i < self->connections; i++) {
        cd = &self->sci_cds[i];
        if (cd->cd_status == UCT_PCIE_CD_READY) {
            cd->cd_status = UCT_PCIE_CD_RESERVED;
            uct_pcie_segment_disconnect(cd->ctl_segment, cd->ctl_segment_map);
            uct_pcie_segment_remove_available(cd->recv_segment,
                                              cd->recv_segment_map);
            cd->cd_status = UCT_PCIE_CD_AVAILABLE;
        }
    }

    SCIRemoveDMAQueue(self->dma_queue, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIRemoveDMAQueue failed: %s", SCIGetErrorString(sci_error));
    }

    uct_pcie_segment_remove(self->dma_segment, self->dma_map);

    uct_pcie_segment_remove_available(self->inbox_segment, self->inbox_map);

    uct_pcie_segment_remove_available(self->ctl_segment, self->ctl_segment_map);

    SCIClose(self->vdev_setup, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_rma, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_recv, UCT_PCIE_NO_FLAGS, &sci_error);

    ucs_arbiter_cleanup(&self->arbiter);

    pthread_mutex_destroy(&self->lock);
}

UCS_CLASS_DEFINE(uct_pcie_iface_t, uct_base_iface_t);

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_pcie_iface_t, uct_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_pcie_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t*);

static ucs_status_t
uct_pcie_query_tl_devices(uct_md_h md, uct_tl_device_resource_t **devices_p,
                          unsigned *num_devices_p)
{
    return uct_single_device_resource(md, UCT_PCIE_NAME, UCT_DEVICE_TYPE_NET,
                                      UCS_SYS_DEVICE_ID_UNKNOWN, devices_p,
                                      num_devices_p);
}

static ucs_status_t
uct_pcie_iface_get_device_address(uct_iface_h tl_iface, uct_device_addr_t *addr)
{
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_device_addr_t *dev_addr = (uct_pcie_device_addr_t*)addr;

    dev_addr->node_id = iface->device_addr;
    return UCS_OK;
}

static ucs_status_t
uct_pcie_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *addr)
{
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_md_t *md       = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    uct_pcie_iface_addr_t *iface_addr = (uct_pcie_iface_addr_t*)addr;

    iface_addr->interrupt_no = iface->interrupt_no;
    iface_addr->inbox_seg_id = iface->inbox_seg_id;
    iface_addr->rma_seg_id   = md->rma_seg_id;
    iface_addr->rma_base     = (uint64_t)(uintptr_t)md->rma_buf;
    return UCS_OK;
}

static ucs_arbiter_cb_result_t
uct_pcie_ep_dispatch_pending(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                             ucs_arbiter_elem_t *elem, void *arg)
{
    uct_pending_req_t *req = ucs_container_of(elem, uct_pending_req_t, priv);
    ucs_status_t status    = req->func(req);

    if (status == UCS_OK) {
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    } else {
        return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
    }
}

/* Move the connect of the first connecting EP along, see
 * uct_pcie_ep_progress_connect */
static UCS_F_NOINLINE unsigned
uct_pcie_iface_progress_connecting(uct_pcie_iface_t *iface)
{
    uct_pcie_ep_t *ep = ucs_list_head(&iface->connecting_eps, uct_pcie_ep_t,
                                      conn_list);
    ucs_status_t status;

    status = uct_pcie_ep_progress_connect(ep);
    if (status == UCS_INPROGRESS) {
        return 0;
    }

    if (status != UCS_OK) {
        uct_iface_handle_ep_err(&iface->super.super, &ep->super.super, status);
    }

    return 1;
}

/* Drain posted packets from each connection's ring in order and ack them,
 * then retry pending sends */
static unsigned uct_pcie_iface_progress(uct_iface_h tl_iface)
{
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    unsigned count          = 0;
    uct_pcie_conn_desc_t *cd;
    uct_pcie_am_hdr_t *packet;
    void *packet_payload_ptr;
    ucs_status_t status;
    uint32_t packet_queue_index;
    uint32_t packet_offset;
    unsigned int i;

    if (ucs_unlikely(!ucs_list_is_empty(&iface->connecting_eps))) {
        count += uct_pcie_iface_progress_connecting(iface);
    }

    for (i = 0; i < iface->connections; i++) {
        cd = &iface->sci_cds[i];

        if (cd->cd_status != UCT_PCIE_CD_READY) {
            continue;
        }

        while (1) {
            packet_queue_index = (cd->ep_conn_last_ack + 1) %
                                 iface->packet_queue_len;
            packet_offset      = iface->packet_size_bytes * packet_queue_index;
            packet = (uct_pcie_am_hdr_t*)&cd->packet_queue_buf[packet_offset];

            /* Stop if the next slot has not been posted by the sender yet */
            if (packet->am_message_posted != 1) {
                break;
            }

            packet_payload_ptr =
                    (void*)&cd->packet_queue_buf[packet_offset +
                                                 sizeof(uct_pcie_am_hdr_t)];

            status = uct_iface_invoke_am(&iface->super, packet->am_id,
                                         packet_payload_ptr, packet->am_length,
                                         0);

            if (status == UCS_INPROGRESS) {
                /* AM handler is not done with the buffer yet, retry on the
                 * next progress call */
                ucs_debug("iface %p am handler %u in progress", iface,
                          packet->am_id);
                break;
            }

            if (status != UCS_OK) {
                ucs_error("iface %p am handler %u failed: %s", iface,
                          packet->am_id, ucs_status_string(status));
                break;
            }

            /* Mark the slot as free and notify the sender by advancing the
             * ack counter */
            packet->am_message_posted = 0;
            cd->ep_conn_last_ack++;
            cd->ctl_buf->ep_conn_ack = cd->ep_conn_last_ack;
            SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

            count++;
        }
    }

    /* The acks above may have freed slots. One request per EP per call, so no
     * EP starves the others. */
    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_pcie_ep_dispatch_pending,
                         NULL);

    return count;
}

static ucs_status_t
uct_pcie_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *attr)
{
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    size_t am_max;

    uct_base_iface_query(ucs_derived_of(tl_iface, uct_base_iface_t), attr);

    attr->cap.flags = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                      UCT_IFACE_FLAG_INTER_NODE | UCT_IFACE_FLAG_CB_SYNC |
                      UCT_IFACE_FLAG_AM_SHORT | UCT_IFACE_FLAG_AM_BCOPY |
                      UCT_IFACE_FLAG_AM_ZCOPY | UCT_IFACE_FLAG_PUT_SHORT |
                      UCT_IFACE_FLAG_PUT_BCOPY | UCT_IFACE_FLAG_PUT_ZCOPY |
                      UCT_IFACE_FLAG_GET_SHORT | UCT_IFACE_FLAG_GET_BCOPY |
                      UCT_IFACE_FLAG_GET_ZCOPY;

    attr->cap.event_flags = 0;

    attr->device_addr_len = sizeof(uct_pcie_device_addr_t);
    attr->iface_addr_len  = sizeof(uct_pcie_iface_addr_t);
    attr->ep_addr_len     = 0;

    /* Each AM slot holds a uct_pcie_am_hdr_t followed by the user data */
    am_max = iface->packet_size_bytes - sizeof(uct_pcie_am_hdr_t);
    attr->cap.am.max_short = am_max;
    attr->cap.am.max_bcopy = am_max;
    attr->cap.am.min_zcopy = 0;
    attr->cap.am.max_zcopy = am_max;
    attr->cap.am.max_iov   = UCT_PCIE_AM_MAX_IOV;
    attr->cap.am.max_hdr   = am_max;

    attr->cap.put.max_short       = UCT_PCIE_MAX_PUT_SHORT;
    attr->cap.put.max_bcopy       = UCT_PCIE_RMA_SEG_SIZE;
    attr->cap.put.min_zcopy       = 0;
    attr->cap.put.max_zcopy       = UCT_PCIE_RMA_SEG_SIZE;
    attr->cap.put.opt_zcopy_align = 1;
    attr->cap.put.align_mtu       = 1;
    attr->cap.put.max_iov         = 1;

    attr->cap.get.max_short       = UCT_PCIE_MAX_PUT_SHORT;
    attr->cap.get.max_bcopy       = UCT_PCIE_RMA_SEG_SIZE;
    attr->cap.get.min_zcopy       = 0;
    attr->cap.get.max_zcopy       = UCT_PCIE_RMA_SEG_SIZE;
    attr->cap.get.opt_zcopy_align = 1;
    attr->cap.get.align_mtu       = 1;
    attr->cap.get.max_iov         = 1;

    /* Conservative PCIe performance estimates: ~1 us for a PCIe round trip
     * and ~20 GB/s for a PCIe 4.0 x16 link. Actual numbers depend on the slot
     * width and generation. */
    attr->latency             = ucs_linear_func_make(1e-6, 0);
    attr->bandwidth.dedicated = 20 * UCS_GBYTE;
    attr->bandwidth.shared    = 0;
    attr->overhead            = 1e-6;
    attr->priority            = 0;

    return UCS_OK;
}

static uct_iface_ops_t uct_pcie_iface_ops = {
    .ep_put_short      = uct_pcie_ep_put_short,
    .ep_put_bcopy      = uct_pcie_ep_put_bcopy,
    .ep_put_zcopy      = uct_pcie_ep_put_zcopy,
    .ep_get_short      = uct_pcie_ep_get_short,
    .ep_get_bcopy      = uct_pcie_ep_get_bcopy,
    .ep_get_zcopy      = uct_pcie_ep_get_zcopy,
    .ep_am_short       = uct_pcie_ep_am_short,
    .ep_am_short_iov   = uct_pcie_ep_am_short_iov,
    .ep_am_bcopy       = uct_pcie_ep_am_bcopy,
    .ep_am_zcopy       = uct_pcie_ep_am_zcopy,
    /* SISCI has no remote atomics, UCP emulates them over AM */
    .ep_atomic_cswap64 = (uct_ep_atomic_cswap64_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic64_post  = (uct_ep_atomic64_post_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch = (uct_ep_atomic64_fetch_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32 = (uct_ep_atomic_cswap32_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic32_post  = (uct_ep_atomic32_post_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch = (uct_ep_atomic32_fetch_func_t)
            ucs_empty_function_return_unsupported,

    .ep_flush          = uct_pcie_ep_flush,
    .ep_fence          = uct_base_ep_fence,
    .ep_check         = (uct_ep_check_func_t)ucs_empty_function_return_success,
    .ep_pending_add   = uct_pcie_ep_pending_add,
    .ep_pending_purge = uct_pcie_ep_pending_purge,
    .ep_create        = UCS_CLASS_NEW_FUNC_NAME(uct_pcie_ep_t),
    .ep_destroy       = UCS_CLASS_DELETE_FUNC_NAME(uct_pcie_ep_t),
    .iface_flush      = uct_base_iface_flush,
    .iface_fence      = uct_base_iface_fence,
    .iface_progress_enable    = uct_base_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_pcie_iface_progress,
    .iface_event_arm          = (uct_iface_event_arm_func_t)
            ucs_empty_function_return_success,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_pcie_iface_t),
    .iface_query              = uct_pcie_iface_query,
    .iface_get_device_address = uct_pcie_iface_get_device_address,
    .iface_get_address        = uct_pcie_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};

#define UCT_PCIE_CONFIG_PREFIX "PCIE_"

UCT_TL_DEFINE(&uct_pcie_component, pcie, uct_pcie_query_tl_devices,
              uct_pcie_iface_t, UCT_PCIE_CONFIG_PREFIX,
              uct_pcie_iface_config_table, uct_pcie_iface_config_t);

UCS_STATIC_INIT
{
    sci_error_t sci_error;

    SCIInitialize(UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIInitialize failed: %s", SCIGetErrorString(sci_error));
    }
}

UCS_STATIC_CLEANUP
{
    SCITerminate();
}
