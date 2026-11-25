/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcie_ep.h"
#include "pcie_md.h"
#include "pcie_sisci_helper.h"

#include <uct/base/uct_iov.inl>

/* Write a connection request into this iface's slot in the peer's inbox and
 * trigger the peer's connection interrupt. Returns UCS_ERR_BUSY, with the
 * reason in busy_reason, if that can't be done yet; the caller tries again on
 * a later call. Moves the EP to UCT_PCIE_EP_CONN_WAITING once the request is
 * written. */
static ucs_status_t
uct_pcie_ep_send_conn_request(uct_pcie_ep_t *ep, const char **busy_reason)
{
    uct_pcie_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_pcie_iface_t);
    unsigned int slot_index = (iface->device_addr / 4) *
                                      UCT_PCIE_MAX_IFACES_PER_NODE +
                              iface->local_index;
    volatile uct_pcie_inbox_slot_t *slot;
    sci_remote_interrupt_t req_interrupt;
    sci_remote_segment_t inbox_segment;
    sci_map_t inbox_map;
    ucs_status_t status;
    sci_error_t sci_error;

    SCIConnectInterrupt(iface->vdev_setup, &req_interrupt, ep->remote_node_id,
                        UCT_PCIE_LOCAL_ADAPTER_NO, ep->remote_interrupt_no,
                        SCI_INFINITE_TIMEOUT, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        *busy_reason = SCIGetErrorString(sci_error);
        return UCS_ERR_BUSY;
    }

    /* Only this iface's slot is mapped */
    status = uct_pcie_segment_try_connect(
            iface->vdev_setup, ep->remote_node_id, ep->remote_inbox_seg_id,
            slot_index * sizeof(uct_pcie_inbox_slot_t),
            sizeof(uct_pcie_inbox_slot_t), NULL, NULL, &inbox_segment,
            &inbox_map, (volatile void**)&slot, &sci_error);
    if (status == UCS_ERR_BUSY) {
        *busy_reason = SCIGetErrorString(sci_error);
        goto out_disconnect_interrupt;
    } else if (status != UCS_OK) {
        goto out_disconnect_interrupt;
    }

    /* A read over PCIe, but only once per connection */
    if (slot->pending) {
        *busy_reason = "the peer has not handled the previous request yet";
        status       = UCS_ERR_BUSY;
        goto out_disconnect_inbox;
    }

    slot->request.node_id        = iface->device_addr;
    slot->request.ctl_segment_id = iface->ctl_segment_id;
    slot->request.ep_conn_index  = ep->ep_conn_index;

    /* The peer checks pending first, so the request has to land before it */
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    slot->pending = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    /* The peer may answer into this EP's ctl slot from now on, even if the
     * trigger below fails */
    ep->conn_step = UCT_PCIE_EP_CONN_WAITING;

    /* Runs uct_pcie_iface_conn_handler on the peer, which answers in this EP's
     * ctl slot */
    SCITriggerInterrupt(req_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCITriggerInterrupt failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
    }

out_disconnect_inbox:
    uct_pcie_segment_disconnect(inbox_segment, inbox_map);
out_disconnect_interrupt:
    SCIDisconnectInterrupt(req_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIDisconnectInterrupt failed: %s",
                 SCIGetErrorString(sci_error));
    }

    return status;
}

/* Runs on the SISCI callback thread. Only disconnect matters: the peer is gone
 * and the EP can't be used anymore. */
static sci_callback_action_t
uct_pcie_ep_remote_segment_cb(void *arg, sci_remote_segment_t segment,
                              sci_segment_cb_reason_t reason,
                              sci_error_t sci_error)
{
    uct_pcie_ep_t *ep = (uct_pcie_ep_t*)arg;

    if (reason == SCI_CB_DISCONNECT) {
        ep->is_connected = 0;
    }

    return SCI_CALLBACK_CONTINUE;
}

static UCS_CLASS_INIT_FUNC(uct_pcie_ep_t, const uct_ep_params_t *params)
{
    const uct_pcie_iface_addr_t *iface_addr = (const uct_pcie_iface_addr_t*)
                                                      params->iface_addr;
    const uct_pcie_device_addr_t *dev_addr  = (const uct_pcie_device_addr_t*)
                                                      params->dev_addr;
    uct_pcie_iface_t *iface = ucs_derived_of(params->iface, uct_pcie_iface_t);
    ucs_status_t status;
    unsigned int ep_conn_index;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);

    self->remote_node_id    = dev_addr->node_id;
    self->super.super.iface = params->iface;
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_debug("ep %p connecting to node %u interrupt %u", self,
              self->remote_node_id, iface_addr->interrupt_no);

    /* This EP's ack slot in the iface control segment. No lock needed,
     * multi-threaded workers are refused at iface init. */
    status = uct_pcie_iface_ctl_slot_get(iface, &ep_conn_index);
    if (status != UCS_OK) {
        ucs_error("iface %p: all %u endpoint slots are in use or closing",
                  iface, iface->max_eps);
        return status;
    }

    self->ep_conn_index   = ep_conn_index;
    self->ep_conn_seq_num = 1;
    ucs_arbiter_group_init(&self->pending_q);

    /* The RMA segment is mapped and the connection request goes out from
     * progress, see uct_pcie_ep_progress_connect */
    self->remote_rma_seg_id   = iface_addr->rma_seg_id;
    self->remote_rma_base     = iface_addr->rma_base;
    self->remote_interrupt_no = iface_addr->interrupt_no;
    self->remote_inbox_seg_id = iface_addr->inbox_seg_id;
    self->conn_step           = UCT_PCIE_EP_CONN_QUEUED;
    self->conn_status         = UCS_INPROGRESS;
    self->is_connected        = 1;
    ucs_list_add_tail(&iface->connecting_eps, &self->conn_list);

    return UCS_OK;
}

/* Called from progress for the first EP in the iface's connecting_eps only.
 * An iface has one slot in each peer's inbox, so one request in flight at a
 * time. */
ucs_status_t uct_pcie_ep_progress_connect(uct_pcie_ep_t *ep)
{
    uct_pcie_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                             uct_pcie_iface_t);
    uct_pcie_ctl_t *ctl     = &iface->ctls[ep->ep_conn_index];
    const char *busy_reason = NULL;
    uint32_t conn_state;
    ucs_status_t status;
    sci_error_t sci_error;

    ucs_assert(ep->conn_status == UCS_INPROGRESS);

    /* The timeout covers mapping the RMA segment, sending the request and
     * waiting for the answer */
    if (ep->conn_step == UCT_PCIE_EP_CONN_QUEUED) {
        ep->conn_step     = UCT_PCIE_EP_CONN_RMA;
        ep->conn_deadline = uct_pcie_connect_deadline();
    }

    /* Map the peer's whole RMA segment for put and get */
    if (ep->conn_step == UCT_PCIE_EP_CONN_RMA) {
        status = uct_pcie_segment_try_connect_full(
                iface->vdev_rma, ep->remote_node_id, ep->remote_rma_seg_id,
                &ep->rma_seg, &ep->rma_seg_map,
                (volatile void**)&ep->remote_rma_va, &sci_error);
        if (status == UCS_ERR_BUSY) {
            if (ucs_get_time() < ep->conn_deadline) {
                return UCS_INPROGRESS;
            }

            ucs_error("ep %p: could not connect to rma segment %u on node %u"
                      " for %d ms: %s", ep, ep->remote_rma_seg_id,
                      ep->remote_node_id, UCT_PCIE_CONNECT_TIMEOUT_MS,
                      SCIGetErrorString(sci_error));
            status = UCS_ERR_ENDPOINT_TIMEOUT;
            goto out;
        } else if (status != UCS_OK) {
            ucs_error("failed to map rma segment %u on node %u",
                      ep->remote_rma_seg_id, ep->remote_node_id);
            goto out;
        }

        ucs_debug("ep %p connected to rma segment %u on node %u, mapped at %p",
                  ep, ep->remote_rma_seg_id, ep->remote_node_id,
                  ep->remote_rma_va);
        ep->conn_step = UCT_PCIE_EP_CONN_SENDING;
    }

    if (ep->conn_step == UCT_PCIE_EP_CONN_SENDING) {
        status = uct_pcie_ep_send_conn_request(ep, &busy_reason);
        if (status == UCS_OK) {
            return UCS_INPROGRESS;
        } else if (status != UCS_ERR_BUSY) {
            goto out;
        } else if (ucs_get_time() < ep->conn_deadline) {
            return UCS_INPROGRESS;
        }

        ucs_error("ep %p: could not send connection request to node %u for"
                  " %d ms: %s", ep, ep->remote_node_id,
                  UCT_PCIE_CONNECT_TIMEOUT_MS, busy_reason);
        status = UCS_ERR_ENDPOINT_TIMEOUT;
        goto out;
    }

    conn_state = ctl->conn_state;
    if (conn_state == UCT_PCIE_CONN_NONE) {
        if (ucs_get_time() < ep->conn_deadline) {
            return UCS_INPROGRESS;
        }

        ucs_error("ep %p: no answer to connection request from node %u after"
                  " %d ms", ep, ep->remote_node_id,
                  UCT_PCIE_CONNECT_TIMEOUT_MS);
        status = UCS_ERR_ENDPOINT_TIMEOUT;
        goto out;
    } else if (conn_state == UCT_PCIE_CONN_REFUSED) {
        ucs_error("ep %p: node %u refused the connection", ep,
                  ep->remote_node_id);
        status = UCS_ERR_REJECTED;
        goto out;
    } else if (conn_state != UCT_PCIE_CONN_ACCEPTED) {
        ucs_error("ep %p: node %u closed the connection before it was set up",
                  ep, ep->remote_node_id);
        status = UCS_ERR_CONNECTION_RESET;
        goto out;
    }

    /* recv_seg_id was written before conn_state */
    ucs_memory_cpu_load_fence();
    ep->remote_seg_id = ctl->recv_seg_id;

    /* Map the receive segment the peer created for this EP (AM ring). Only
     * before the deadline: the peer frees a connection that isn't mapped by
     * twice the timeout after answering, see uct_pcie_iface_accept. */
    status = uct_pcie_segment_try_connect(
            iface->vdev_ep, ep->remote_node_id, ep->remote_seg_id, 0,
            iface->packet_size_bytes * iface->packet_queue_len,
            uct_pcie_ep_remote_segment_cb, ep, &ep->remote_segment,
            &ep->remote_seg_map, (volatile void**)&ep->remote_seg_buf,
            &sci_error);
    if (status == UCS_ERR_BUSY) {
        if (ucs_get_time() < ep->conn_deadline) {
            return UCS_INPROGRESS;
        }

        ucs_error("ep %p: could not connect to receive segment %u on node %u"
                  " for %d ms: %s", ep, ep->remote_seg_id, ep->remote_node_id,
                  UCT_PCIE_CONNECT_TIMEOUT_MS, SCIGetErrorString(sci_error));
        status = UCS_ERR_ENDPOINT_TIMEOUT;
        goto out;
    } else if (status != UCS_OK) {
        ucs_error("failed to map receive segment %u on node %u",
                  ep->remote_seg_id, ep->remote_node_id);
        goto out;
    }

    ucs_debug("ep %p connected to receive segment %u on node %u", ep,
              ep->remote_seg_id, ep->remote_node_id);

    /* Sends queued while connecting can go now */
    ucs_arbiter_group_schedule(&iface->arbiter, &ep->pending_q);

out:
    ucs_list_del(&ep->conn_list);
    ep->conn_status = status;
    return status;
}

/* UCP purges pending requests before destroying an EP, so this shouldn't run */
static ucs_arbiter_cb_result_t
uct_pcie_ep_pending_discard_cb(ucs_arbiter_t *arbiter,
                               ucs_arbiter_group_t *group,
                               ucs_arbiter_elem_t *elem, void *arg)
{
    ucs_warn("ep destroyed with a pending request still queued");
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

static UCS_CLASS_CLEANUP_FUNC(uct_pcie_ep_t)
{
    uct_pcie_iface_t *iface = ucs_derived_of(self->super.super.iface,
                                             uct_pcie_iface_t);

    ucs_arbiter_group_purge(&iface->arbiter, &self->pending_q,
                            uct_pcie_ep_pending_discard_cb, NULL);

    if (self->conn_status == UCS_INPROGRESS) {
        ucs_list_del(&self->conn_list);
    }

    /* rma_seg is mapped once the EP gets to SENDING */
    if ((self->conn_step == UCT_PCIE_EP_CONN_SENDING) ||
        (self->conn_step == UCT_PCIE_EP_CONN_WAITING)) {
        uct_pcie_segment_disconnect(self->rma_seg, self->rma_seg_map);
    }

    if (self->conn_status == UCS_OK) {
        self->remote_seg_buf = NULL;
        uct_pcie_segment_disconnect(self->remote_segment,
                                    self->remote_seg_map);
        ucs_debug("ep %p destroyed, receive segment %u on node %u", self,
                  self->remote_seg_id, self->remote_node_id);
    } else {
        ucs_debug("ep %p destroyed before connecting to node %u: %s", self,
                  self->remote_node_id, ucs_status_string(self->conn_status));
    }

    uct_pcie_iface_ctl_slot_put(iface, self->ep_conn_index,
                                self->conn_step == UCT_PCIE_EP_CONN_WAITING);
}

UCS_CLASS_DEFINE(uct_pcie_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NEW_FUNC(uct_pcie_ep_t, uct_ep_t, const uct_ep_params_t*);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_pcie_ep_t, uct_ep_t);

/* Sends, puts and gets need the whole connect done, see conn_status */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_pcie_ep_check_conn(const uct_pcie_ep_t *ep)
{
    if (ucs_likely(ep->conn_status == UCS_OK)) {
        return UCS_OK;
    }

    return (ep->conn_status == UCS_INPROGRESS) ? UCS_ERR_NO_RESOURCE :
                                                 ep->conn_status;
}

/*
 * remote_addr in the put/get functions below is a VA in the peer's RMA
 * segment. It is turned into a pointer into the local mapping of that segment:
 *
 *   seg_offset = remote_addr - ep->remote_rma_base
 *   local_ptr  = ep->remote_rma_va + seg_offset
 */

ucs_status_t uct_pcie_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                   unsigned length, uint64_t remote_addr,
                                   uct_rkey_t rkey)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    ucs_status_t status;
    size_t seg_offset;

    UCT_CHECK_LENGTH(length, 0, UCT_PCIE_MAX_PUT_SHORT, "put_short");

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    ucs_assert(remote_addr >= ep->remote_rma_base);
    seg_offset = (size_t)(remote_addr - ep->remote_rma_base);
    ucs_assert(seg_offset + length <= UCT_PCIE_RMA_SEG_SIZE);

    memcpy(UCS_PTR_BYTE_OFFSET(ep->remote_rma_va, seg_offset), buffer, length);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return UCS_OK;
}

ssize_t uct_pcie_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                              void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    ucs_status_t status;
    size_t seg_offset;
    ssize_t length;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    ucs_assert(remote_addr >= ep->remote_rma_base);
    seg_offset = (size_t)(remote_addr - ep->remote_rma_base);

    length = pack_cb(UCS_PTR_BYTE_OFFSET(ep->remote_rma_va, seg_offset), arg);
    ucs_assert(seg_offset + (size_t)length <= UCT_PCIE_RMA_SEG_SIZE);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return length;
}

/* Blocking DMA between a local and a remote segment. flags is
 * SCI_FLAG_DMA_READ for get and UCT_PCIE_NO_FLAGS for put. */
static ucs_status_t
uct_pcie_iface_dma_transfer(uct_pcie_iface_t *iface,
                            sci_local_segment_t local_seg, size_t local_offset,
                            sci_remote_segment_t remote_seg,
                            size_t remote_offset, size_t length, unsigned flags)
{
    const char *op = (flags & SCI_FLAG_DMA_READ) ? "get" : "put";
    sci_error_t sci_error;
    sci_dma_queue_state_t dma_state;

    SCIStartDmaTransfer(iface->dma_queue, local_seg, remote_seg, local_offset,
                        length, remote_offset, NULL, NULL, flags, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIStartDmaTransfer (%s) failed: %s, falling back to PIO", op,
                 SCIGetErrorString(sci_error));
        return UCS_ERR_IO_ERROR;
    }

    /* sci_error only covers the wait call, the transfer result is dma_state */
    dma_state = SCIWaitForDMAQueue(iface->dma_queue, SCI_INFINITE_TIMEOUT,
                                   UCT_PCIE_NO_FLAGS, &sci_error);
    if ((sci_error != SCI_ERR_OK) || (dma_state != SCI_DMAQUEUE_DONE)) {
        ucs_warn("SCIWaitForDMAQueue (%s) failed: %s, state %d, using PIO", op,
                 SCIGetErrorString(sci_error), dma_state);
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

ucs_status_t uct_pcie_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                   size_t iovcnt, uint64_t remote_addr,
                                   uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_md_t *md       = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    ucs_iov_iter_t iov_iter;
    size_t seg_offset;
    size_t total_len;
    ptrdiff_t src_offset;
    ucs_status_t status;
    int src_in_pool;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    ucs_assert(remote_addr >= ep->remote_rma_base);
    seg_offset = (size_t)(remote_addr - ep->remote_rma_base);

    total_len = uct_iov_total_length(iov, iovcnt);
    ucs_assert(seg_offset + total_len <= UCT_PCIE_RMA_SEG_SIZE);

    /* Pool-backed source: DMA segment to segment, no local copy. The
     * range check stays behind the threshold so small puts don't pay it. */
    if ((total_len > UCT_PCIE_PUT_DMA_THRESHOLD) && (iovcnt == 1) &&
        (iov[0].count == 1)) {
        src_offset  = UCS_PTR_BYTE_DIFF(md->rma_buf, iov[0].buffer);
        src_in_pool = (src_offset >= 0) &&
                      ((size_t)src_offset + total_len <= UCT_PCIE_RMA_SEG_SIZE);
        if (src_in_pool &&
            (uct_pcie_iface_dma_transfer(iface, md->rma_seg, (size_t)src_offset,
                                         ep->rma_seg, seg_offset, total_len,
                                         UCT_PCIE_NO_FLAGS) == UCS_OK)) {
            return UCS_OK;
        }
    }

    ucs_iov_iter_init(&iov_iter);
    uct_iov_to_buffer(iov, iovcnt, &iov_iter,
                      UCS_PTR_BYTE_OFFSET(ep->remote_rma_va, seg_offset),
                      total_len);

    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return UCS_OK;
}

/* Return where to read remote_addr from: the DMA staging buffer, or the PIO
 * window if the read is small or DMA fails */
static const void *uct_pcie_ep_read_src(uct_pcie_ep_t *ep,
                                        uct_pcie_iface_t *iface,
                                        uint64_t remote_addr, size_t length)
{
    size_t seg_offset;

    ucs_assert(remote_addr >= ep->remote_rma_base);
    seg_offset = (size_t)(remote_addr - ep->remote_rma_base);
    ucs_assert(seg_offset + length <= UCT_PCIE_RMA_SEG_SIZE);

    if ((length > UCT_PCIE_GET_DMA_THRESHOLD) &&
        (uct_pcie_iface_dma_transfer(iface, iface->dma_segment, 0, ep->rma_seg,
                                     seg_offset, length,
                                     SCI_FLAG_DMA_READ) == UCS_OK)) {
        return iface->dma_buffer;
    }

    return (const uint8_t*)ep->remote_rma_va + seg_offset;
}

/* Read length bytes at remote_addr into dest */
static void uct_pcie_ep_read(uct_pcie_ep_t *ep, uct_pcie_iface_t *iface,
                             uint64_t remote_addr, size_t length, void *dest)
{
    uct_pcie_md_t *md     = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    ptrdiff_t dest_offset = UCS_PTR_BYTE_DIFF(md->rma_buf, dest);
    size_t seg_offset;
    int dest_in_pool;

    ucs_assert(remote_addr >= ep->remote_rma_base);
    seg_offset = (size_t)(remote_addr - ep->remote_rma_base);
    ucs_assert(seg_offset + length <= UCT_PCIE_RMA_SEG_SIZE);

    dest_in_pool = (dest_offset >= 0) &&
                   ((size_t)dest_offset + length <= UCT_PCIE_RMA_SEG_SIZE);

    /* Small reads: PIO */
    if (length <= UCT_PCIE_GET_DMA_THRESHOLD) {
        memcpy(dest, (const uint8_t*)ep->remote_rma_va + seg_offset, length);
        return;
    }

    /* dest is in the MD pool: DMA straight into it */
    if (dest_in_pool &&
        (uct_pcie_iface_dma_transfer(iface, md->rma_seg, (size_t)dest_offset,
                                     ep->rma_seg, seg_offset, length,
                                     SCI_FLAG_DMA_READ) == UCS_OK)) {
        return;
    }

    /* Otherwise, or if that failed: DMA into the staging segment, then copy */
    if (uct_pcie_iface_dma_transfer(iface, iface->dma_segment, 0, ep->rma_seg,
                                    seg_offset, length,
                                    SCI_FLAG_DMA_READ) == UCS_OK) {
        memcpy(dest, iface->dma_buffer, length);
        return;
    }

    /* DMA failed: PIO */
    memcpy(dest, (const uint8_t*)ep->remote_rma_va + seg_offset, length);
}

ucs_status_t uct_pcie_ep_get_short(uct_ep_h tl_ep, void *buffer,
                                   unsigned length, uint64_t remote_addr,
                                   uct_rkey_t rkey)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    ucs_status_t status;

    UCT_CHECK_LENGTH(length, 0, UCT_PCIE_MAX_PUT_SHORT, "get_short");

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    uct_pcie_ep_read(ep, iface, remote_addr, length, buffer);
    return UCS_OK;
}

ucs_status_t uct_pcie_ep_get_bcopy(uct_ep_h tl_ep,
                                   uct_unpack_callback_t unpack_cb, void *arg,
                                   size_t length, uint64_t remote_addr,
                                   uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    ucs_status_t status;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    unpack_cb(arg, uct_pcie_ep_read_src(ep, iface, remote_addr, length),
              length);
    return UCS_OK;
}

ucs_status_t uct_pcie_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                   size_t iovcnt, uint64_t remote_addr,
                                   uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uint64_t addr           = remote_addr;
    ucs_status_t status;
    size_t i;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    for (i = 0; i < iovcnt; i++) {
        uct_pcie_ep_read(ep, iface, addr, iov[i].length, iov[i].buffer);
        addr += iov[i].length;
    }

    return UCS_OK;
}

/* Slot layout: [ uct_pcie_am_hdr_t | uint64_t header | payload ] */
ucs_status_t uct_pcie_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t *ctl     = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    uint32_t slot_offset;
    uint32_t payload_offset;
    ucs_status_t status;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    /* No free slot until the peer acks */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_CHECK_LENGTH(sizeof(uct_pcie_am_hdr_t) + sizeof(uint64_t) + length, 0,
                     iface->packet_size_bytes, "am_short");
    UCT_CHECK_AM_ID(id);

    slot_offset    = iface->packet_size_bytes *
                     (ep->ep_conn_seq_num % iface->packet_queue_len);
    payload_offset = slot_offset + sizeof(uct_pcie_am_hdr_t);

    uct_am_short_fill_data((void*)&ep->remote_seg_buf[payload_offset], header,
                           payload, length, UCS_ARCH_MEMCPY_NT_DEST);

    /* Flush so the payload lands before the posted flag */
    am_hdr            = (uct_pcie_am_hdr_t*)&ep->remote_seg_buf[slot_offset];
    am_hdr->am_id     = id;
    am_hdr->am_length = length + sizeof(header);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return UCS_OK;
}

/* Slot layout: [ uct_pcie_am_hdr_t | iov[0] | iov[1] | ... ] */
ucs_status_t uct_pcie_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                      const uct_iov_t *iov, size_t iovcnt)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t *ctl     = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    ucs_iov_iter_t iov_iter;
    uint32_t slot_offset;
    uint32_t payload_offset;
    size_t iov_total_len;
    size_t bytes_copied;
    ucs_status_t status;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    /* No free slot until the peer acks */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    iov_total_len = uct_iov_total_length(iov, iovcnt);

    UCT_CHECK_LENGTH(iov_total_len + sizeof(uct_pcie_am_hdr_t), 0,
                     iface->packet_size_bytes, "am_short_iov");
    UCT_CHECK_AM_ID(id);

    slot_offset    = iface->packet_size_bytes *
                     (ep->ep_conn_seq_num % iface->packet_queue_len);
    payload_offset = slot_offset + sizeof(uct_pcie_am_hdr_t);

    ucs_iov_iter_init(&iov_iter);
    bytes_copied = uct_iov_to_buffer(iov, iovcnt, &iov_iter,
                                     (void*)&ep->remote_seg_buf[payload_offset],
                                     iov_total_len);
    ucs_assert(bytes_copied == iov_total_len);

    /* Flush so the payload lands before the posted flag */
    am_hdr            = (uct_pcie_am_hdr_t*)&ep->remote_seg_buf[slot_offset];
    am_hdr->am_id     = id;
    am_hdr->am_length = iov_total_len;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return UCS_OK;
}

/* Slot layout: [ uct_pcie_am_hdr_t | packed payload ] */
ssize_t uct_pcie_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t *ctl     = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    uint32_t slot_offset;
    uint32_t payload_offset;
    ssize_t length;
    ucs_status_t status;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    /* No free slot until the peer acks */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_CHECK_AM_ID(id);

    slot_offset    = iface->packet_size_bytes *
                     (ep->ep_conn_seq_num % iface->packet_queue_len);
    payload_offset = slot_offset + sizeof(uct_pcie_am_hdr_t);

    am_hdr = (uct_pcie_am_hdr_t*)&ep->remote_seg_buf[slot_offset];

    length = pack_cb((void*)&ep->remote_seg_buf[payload_offset], arg);

    /* Flush so the payload lands before the posted flag */
    am_hdr->am_id     = id;
    am_hdr->am_length = length;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return length;
}

/* Slot layout: [ uct_pcie_am_hdr_t | header | iov payload ] */
ucs_status_t uct_pcie_ep_am_zcopy(uct_ep_h uct_ep, uint8_t id,
                                  const void *header, unsigned header_length,
                                  const uct_iov_t *iov, size_t iovcnt,
                                  unsigned flags, uct_completion_t *comp)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(uct_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(uct_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t *ctl     = &iface->ctls[ep->ep_conn_index];
    size_t iov_total_len    = uct_iov_total_length(iov, iovcnt);
    size_t total_size       = sizeof(uct_pcie_am_hdr_t) + header_length +
                              iov_total_len;
    uct_pcie_am_hdr_t *am_hdr;
    uint8_t *dest_hdr;
    void *dest_payload;
    ucs_iov_iter_t iov_iter;
    size_t bytes_copied;
    uint32_t slot_offset;
    ucs_status_t status;

    status = uct_pcie_ep_check_conn(ep);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    /* No free slot until the peer acks */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_CHECK_LENGTH(total_size, 0, iface->packet_size_bytes, "am_zcopy");
    UCT_CHECK_AM_ID(id);

    slot_offset = iface->packet_size_bytes *
                  (ep->ep_conn_seq_num % iface->packet_queue_len);

    am_hdr       = (uct_pcie_am_hdr_t*)&ep->remote_seg_buf[slot_offset];
    dest_hdr     = (uint8_t*)&am_hdr[1];
    dest_payload = dest_hdr + header_length;

    am_hdr->am_id     = id;
    am_hdr->am_length = header_length + iov_total_len;

    memcpy(dest_hdr, header, header_length);

    ucs_iov_iter_init(&iov_iter);
    bytes_copied = uct_iov_to_buffer(iov, iovcnt, &iov_iter, dest_payload,
                                     iov_total_len);
    ucs_assert(bytes_copied == iov_total_len);

    /* Flush so the payload lands before the posted flag */
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return UCS_OK;
}

typedef struct {
    uct_pending_purge_callback_t cb;
    void                         *arg;
} uct_pcie_pending_purge_args_t;

static ucs_arbiter_cb_result_t
uct_pcie_ep_pending_purge_cb(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                             ucs_arbiter_elem_t *elem, void *arg)
{
    uct_pcie_pending_purge_args_t *args = arg;
    uct_pending_req_t *req = ucs_container_of(elem, uct_pending_req_t, priv);

    if (args->cb != NULL) {
        args->cb(req, args->arg);
    }

    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

ucs_status_t
uct_pcie_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req, unsigned flags)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t *ctl     = &iface->ctls[ep->ep_conn_index];

    /* Failed EP: the caller retries and gets the error from the send */
    if (UCS_STATUS_IS_ERR(ep->conn_status)) {
        return UCS_ERR_BUSY;
    }

    /* Connected, room in the window and nothing queued ahead: the caller
     * retries */
    if ((ep->conn_status == UCS_OK) &&
        ucs_arbiter_group_is_empty(&ep->pending_q) &&
        (ep->ep_conn_seq_num - ctl->ep_conn_ack <= iface->packet_queue_len)) {
        return UCS_ERR_BUSY;
    }

    uct_pending_req_arb_group_push(&ep->pending_q, req);

    /* While connecting, uct_pcie_ep_progress_connect schedules the group */
    if (ep->conn_status == UCS_OK) {
        ucs_arbiter_group_schedule(&iface->arbiter, &ep->pending_q);
    }

    return UCS_OK;
}

void uct_pcie_ep_pending_purge(uct_ep_h tl_ep, uct_pending_purge_callback_t cb,
                               void *arg)
{
    uct_pcie_ep_t *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_pending_purge_args_t args = {
        .cb  = cb,
        .arg = arg
    };

    ucs_arbiter_group_purge(&iface->arbiter, &ep->pending_q,
                            uct_pcie_ep_pending_purge_cb, &args);
}

ucs_status_t uct_pcie_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    ucs_status_t status;

    /* Sends are done when they return, so only sends that haven't gone out
     * yet are left to wait for */
    if (!(flags & UCT_FLUSH_FLAG_CANCEL)) {
        status = uct_pcie_ep_check_conn(ep);
        if (status != UCS_OK) {
            return status;
        }

        if (!ucs_arbiter_group_is_empty(&ep->pending_q)) {
            return UCS_ERR_NO_RESOURCE;
        }
    }

    return uct_base_ep_flush(tl_ep, flags, comp);
}
