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

/* Send a connection request to the iface listening on remote_interrupt_no */
static ucs_status_t
uct_pcie_ep_send_conn_request(uct_pcie_conn_req_t *request,
                              unsigned int node_id,
                              unsigned int remote_interrupt_no,
                              sci_desc_t sci_virtual_device)
{
    ucs_status_t status = UCS_OK;
    sci_remote_data_interrupt_t req_interrupt;
    ucs_time_t deadline;
    sci_error_t sci_error;

    deadline = uct_pcie_connect_deadline();
    do {
        SCIConnectDataInterrupt(sci_virtual_device, &req_interrupt, node_id,
                                UCT_PCIE_LOCAL_ADAPTER_NO, remote_interrupt_no,
                                SCI_INFINITE_TIMEOUT, UCT_PCIE_NO_FLAGS,
                                &sci_error);
    } while ((sci_error != SCI_ERR_OK) && (ucs_get_time() < deadline));

    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIConnectDataInterrupt failed after %d ms: %s",
                  UCT_PCIE_CONNECT_TIMEOUT_MS, SCIGetErrorString(sci_error));
        return UCS_ERR_TIMED_OUT;
    }

    SCITriggerDataInterrupt(req_interrupt, (void*)request, sizeof(*request),
                            UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCITriggerDataInterrupt failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
    }

    SCIDisconnectDataInterrupt(req_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIDisconnectDataInterrupt failed: %s",
                 SCIGetErrorString(sci_error));
    }

    return status;
}

/* Connection handshake: send the request along with a reply interrupt, then
 * wait for the answer carrying the peer's receive segment ID */
static ucs_status_t
uct_pcie_ep_send_recv_conn_request(uct_pcie_conn_req_t request,
                                   unsigned int node_id,
                                   unsigned int remote_interrupt_no,
                                   sci_desc_t sci_virtual_device,
                                   uct_pcie_conn_ans_t *answer)
{
    unsigned int ans_size = sizeof(*answer);
    sci_local_data_interrupt_t ans_interrupt;
    ucs_status_t status;
    sci_error_t sci_error;

    /* Reply interrupt, SISCI picks a free number that goes in the request */
    SCICreateDataInterrupt(sci_virtual_device, &ans_interrupt,
                           UCT_PCIE_LOCAL_ADAPTER_NO, &request.interrupt, NULL,
                           NULL, /* No callback arguments */
                           UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCICreateDataInterrupt failed: %s",
                  SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }

    /* Runs uct_pcie_iface_conn_handler on the peer, which sends the answer */
    status = uct_pcie_ep_send_conn_request(&request, node_id,
                                           remote_interrupt_no,
                                           sci_virtual_device);
    if (status != UCS_OK) {
        goto out_remove_interrupt;
    }

    /* Wait for the answer */
    SCIWaitForDataInterrupt(ans_interrupt, (void*)answer, &ans_size,
                            UCT_PCIE_CONNECT_TIMEOUT_MS, UCT_PCIE_NO_FLAGS,
                            &sci_error);
    if (sci_error == SCI_ERR_TIMEOUT) {
        ucs_error("no answer to connection request after %d ms",
                  UCT_PCIE_CONNECT_TIMEOUT_MS);
        status = UCS_ERR_TIMED_OUT;
    } else if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIWaitForDataInterrupt failed: %s",
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
    }

out_remove_interrupt:
    SCIRemoveDataInterrupt(ans_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);
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
    uct_pcie_md_t *md       = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    uct_pcie_conn_ans_t answer;
    uct_pcie_conn_req_t request;
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
    ep_conn_index = iface->eps_init_cnt++;

    request.node_id        = iface->device_addr;
    request.ep_conn_index  = ep_conn_index;
    request.ctl_segment_id = iface->ctl_segment_id;

    status = uct_pcie_ep_send_recv_conn_request(request, self->remote_node_id,
                                                iface_addr->interrupt_no,
                                                md->sci_virtual_device,
                                                &answer);
    if (status != UCS_OK) {
        goto err_dec_eps_init_cnt;
    }

    self->remote_seg_id   = answer.segment_id;
    self->ep_conn_index   = ep_conn_index;
    self->ep_conn_seq_num = 1;
    ucs_arbiter_group_init(&self->pending_q);

    /* Map the peer's whole RMA segment for put and get */
    status = uct_pcie_segment_connect_full(
            iface->vdev_rma, self->remote_node_id, iface_addr->rma_seg_id,
            &self->rma_seg, &self->rma_seg_map,
            (volatile void**)&self->remote_rma_va);
    if (status != UCS_OK) {
        ucs_error("failed to connect to rma segment %u on node %u",
                  iface_addr->rma_seg_id, self->remote_node_id);
        goto err_dec_eps_init_cnt;
    }

    self->remote_rma_base = iface_addr->rma_base;

    ucs_debug("ep %p connected to rma segment %u on node %u, mapped at %p",
              self, iface_addr->rma_seg_id, self->remote_node_id,
              self->remote_rma_va);

    /* Map the receive segment the peer created for this EP (AM ring) */
    status = uct_pcie_segment_connect(
            iface->vdev_ep, self->remote_node_id, self->remote_seg_id, 0,
            iface->packet_size_bytes * iface->packet_queue_len,
            uct_pcie_ep_remote_segment_cb, self, &self->remote_segment,
            &self->remote_seg_map, (volatile void**)&self->remote_seg_buf);
    if (status != UCS_OK) {
        ucs_error("failed to connect to receive segment %u on node %u",
                  self->remote_seg_id, self->remote_node_id);
        goto err_disconnect_rma_seg;
    }

    self->is_connected = 1;

    ucs_debug("ep %p connected to receive segment %u on node %u", self,
              self->remote_seg_id, self->remote_node_id);

    return UCS_OK;

err_disconnect_rma_seg:
    uct_pcie_segment_disconnect(self->rma_seg, self->rma_seg_map);
err_dec_eps_init_cnt:
    iface->eps_init_cnt--;
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

    uct_pcie_segment_disconnect(self->rma_seg, self->rma_seg_map);
    self->remote_seg_buf = NULL;
    uct_pcie_segment_disconnect(self->remote_segment, self->remote_seg_map);

    ucs_debug("ep %p destroyed, receive segment %u on node %u", self,
              self->remote_seg_id, self->remote_node_id);
}

UCS_CLASS_DEFINE(uct_pcie_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NEW_FUNC(uct_pcie_ep_t, uct_ep_t, const uct_ep_params_t*);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_pcie_ep_t, uct_ep_t);

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
    size_t seg_offset;

    UCT_CHECK_LENGTH(length, 0, UCT_PCIE_MAX_PUT_SHORT, "put_short");

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
    size_t seg_offset;
    ssize_t length;

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
    int src_in_pool;

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

    UCT_CHECK_LENGTH(length, 0, UCT_PCIE_MAX_PUT_SHORT, "get_short");

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
    size_t i;

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

    /* Room in the window and nothing queued ahead, the caller retries */
    if (ucs_arbiter_group_is_empty(&ep->pending_q) &&
        (ep->ep_conn_seq_num - ctl->ep_conn_ack <= iface->packet_queue_len)) {
        return UCS_ERR_BUSY;
    }

    uct_pending_req_arb_group_push(&ep->pending_q, req);

    ucs_arbiter_group_schedule(&iface->arbiter, &ep->pending_q);

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
