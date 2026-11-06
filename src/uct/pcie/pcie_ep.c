

#include <uct/base/uct_iov.inl>

#include "pcie_ep.h"
#include "pcie_sisci_helper.h"
#include "pcie_md.h"

#include <sys/mman.h>

/**
 * @brief Send a connection request to a remote iface.
 *
 * Connects to the remote iface's data interrupt (identified by
 * remote_interrupt_no on node_id), triggers it with the request struct,
 * then disconnects. The request carries the information the remote iface
 * needs to complete the connection: local node ID, ctl segment ID, EP slot
 * index, and the reply interrupt number to send the answer back to.
 *
 * @param[in] request              Endpoint data the remote iface needs to complete
 *                                 the connection (node ID, ctl segment, slot index,
 *                                 reply interrupt number).
 * @param[in] node_id              SISCI node ID of the target iface.
 * @param[in] remote_interrupt_no  Interrupt number exported by the target iface.
 * @param[in] sci_virtual_device   SISCI descriptor (used to create SISCI resources).
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE if the trigger failed.
 */
static ucs_status_t uct_pcie_ep_send_conn_request(
    uct_pcie_conn_req_t *request,
    unsigned int node_id,
    unsigned int remote_interrupt_no,
    sci_desc_t sci_virtual_device)
{
    sci_error_t sci_error;
    sci_remote_data_interrupt_t req_interrupt;
    int rc = UCS_OK;

    do {
        SCIConnectDataInterrupt(
            sci_virtual_device,
            &req_interrupt,
            node_id,
            UCT_PCIE_LOCAL_ADAPTER_NO,
            remote_interrupt_no,
            SCI_INFINITE_TIMEOUT,
            UCT_PCIE_NO_FLAGS,
            &sci_error);
    } while (sci_error != SCI_ERR_OK);

    SCITriggerDataInterrupt(
        req_interrupt,
        (void *) request,
        sizeof(uct_pcie_conn_req_t),
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCI Trigger Interrupt: %s", SCIGetErrorString(sci_error));
        rc = UCS_ERR_NO_RESOURCE;
    }

    /*  Clean up for connection.  */
    SCIDisconnectDataInterrupt(req_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);
    if(sci_error == SCI_ERR_BUSY) {
        ucs_error("SCIDisconnectDataInterrupt: %s",
            SCIGetErrorString(sci_error));
    }

    return rc;
}

/**
 * @brief Perform the full connection request/response handshake.
 *
 * Creates a local reply interrupt so the remote iface knows where to send
 * the answer, sends the connection request via uct_pcie_ep_send_conn_request,
 * then blocks on the reply interrupt until the answer arrives. The answer
 * carries the remote segment ID and the offset within it that this EP owns.
 *
 * @param[in]  request              Connection request (node_id, ctl_segment_id,
 *                                  ep_conn_index, and reply interrupt filled in).
 * @param[in]  node_id              SISCI node ID of the target iface.
 * @param[in]  remote_interrupt_no  Connection interrupt number of the target iface.
 * @param[in]  sci_virtual_device   Local SISCI descriptor.
 * @param[out] answer               Filled with segment_id and offset on success.
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE on any SISCI failure.
 */
static ucs_status_t uct_pcie_ep_send_recv_conn_request(
    uct_pcie_conn_req_t request,
    unsigned int node_id,
    unsigned int remote_interrupt_no,
    sci_desc_t sci_virtual_device,
    uct_pcie_conn_ans_t *answer)
{
    sci_error_t sci_error = SCI_ERR_OK;
    ucs_status_t ucs_ret = UCS_OK;
    sci_local_data_interrupt_t ans_interrupt;
    unsigned int ans_size;
    unsigned int attempt;

    /* 1.
     * Create the data interrupt we will use to receive the response of our
     * connection request. We get sisci to give us a free interrupt id.
     * We send that ID along with our connection request such that the server
     * can get back to us. */
    SCICreateDataInterrupt(
        sci_virtual_device,
        &ans_interrupt,
        UCT_PCIE_LOCAL_ADAPTER_NO,
        &request.interrupt,  
        UCT_PCIE_NO_CALLBACK,
        NULL, /* No callback arguments */
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCI Trigger Interrupt: %s", SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }
    
    for (attempt = 0; attempt < UCT_PCIE_CONN_ATTEMPTS; attempt++) {
        /* 2.
         * Send connection request to the server. This will trigger a callback
         * on the other side, and that callback will get back to us and send
         * us an answer to our connection request. A repeated request gets the
         * same answer (see uct_pcie_conn_handler). */
        ucs_ret = uct_pcie_ep_send_conn_request(
            &request,
            node_id,
            remote_interrupt_no,
            sci_virtual_device);
        if (ucs_ret != UCS_OK) {
            break;
        }

        /* 3. Wait for connection request answer */
        ans_size = (unsigned int) sizeof(uct_pcie_conn_ans_t);
        SCIWaitForDataInterrupt(
            ans_interrupt,
            (void*) answer,
            &ans_size,
            UCT_PCIE_CONN_WAIT_MS,
            UCT_PCIE_NO_FLAGS,
            &sci_error);
        if (sci_error != SCI_ERR_TIMEOUT) {
            break;
        }
        ucs_warn("pcie: no answer from node %u interrupt %u in %d ms, asking "
                 "again", node_id, remote_interrupt_no, UCT_PCIE_CONN_WAIT_MS);
    }
    if ((ucs_ret == UCS_OK) && (sci_error != SCI_ERR_OK)) {
        ucs_error("SCI Wait For Interrupt: %s", SCIGetErrorString(sci_error));
        ucs_ret = UCS_ERR_NO_RESOURCE;
    }
    /* Done. Clean up data interrupt made in step 1 */
    SCIRemoveDataInterrupt(ans_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);

    return ucs_ret;
}

/* Fires from SISCI's interrupt thread when the remote segment changes state.
 * We only care about disconnect: the remote side has gone away and the EP is
 * no longer usable. All other reasons are ignored. */
static sci_callback_action_t
uct_pcie_ep_remote_segment_cb(void *arg,
                               sci_remote_segment_t segment,
                               sci_segment_cb_reason_t reason,
                               sci_error_t status)
{
    uct_pcie_ep_t *ep = (uct_pcie_ep_t *)arg;
    if (reason == SCI_CB_DISCONNECT) {
        ep->is_connected = 0;
    }
    return SCI_CALLBACK_CONTINUE;
}

/**
 * @brief Construct endpoint and complete the connection handshake.
 *
 * Claims a slot in the iface's pre-allocated control segment by incrementing
 * the iface's EP init counter, initiates the request/response handshake with
 * the remote iface to obtain a receive segment ID, then connects to the
 * dedicated receive segment (for AM sends). Put/get need no per-EP setup:
 * their target segments come with the rkeys.
 *
 * self is a uct_pcie_ep_t *   - injected by the UCS_CLASS_INIT_FUNC macro.
 */
static UCS_CLASS_INIT_FUNC(uct_pcie_ep_t, const uct_ep_params_t *params)
{
    uct_pcie_iface_addr_t *iface_addr = (uct_pcie_iface_addr_t *)params->iface_addr;
    uct_pcie_device_addr_t *dev_addr  = (uct_pcie_device_addr_t *)params->dev_addr;
    uct_pcie_iface_t *iface = ucs_derived_of(params->iface, uct_pcie_iface_t);
    uct_pcie_md_t    *md    = ucs_derived_of(iface->super.md,  uct_pcie_md_t);

    uct_pcie_conn_ans_t answer;
    uct_pcie_conn_req_t request;
    ucs_status_t        ucs_ret;
    unsigned int        ep_conn_index;
    int                 ret;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);

    /* --- Extract remote address info from params --- */
    self->remote_node_id = (unsigned int)dev_addr->node_id;
    self->super.super.iface = params->iface;
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    ucs_debug("EP created remote_interrupt_no %d node_id %d\n",
              iface_addr->interrupt_no, self->remote_node_id);

    /* --- Assign a slot index for this EP --- */
    /* ep_conn_index is used to index into the iface's control segment and to
     * identify this EP to the remote iface during the handshake. No lock here:
     * UCX guarantees EP creation is single-threaded per worker. */
    ep_conn_index = iface->eps_init_cnt++;

    /* --- Connection handshake --- */
    request.node_id        = iface->device_addr;
    request.ep_conn_index  = ep_conn_index;
    request.ctl_segment_id = iface->ctl_segment_id;

    ucs_ret = uct_pcie_ep_send_recv_conn_request(
        request,
        self->remote_node_id,
        (unsigned int)iface_addr->interrupt_no,
        md->sci_virtual_device,
        &answer);
    if (ucs_ret != UCS_OK) {
        iface->eps_init_cnt--;
        return ucs_ret;
    }

    /* uct_pcie_ep_t *self: store handshake results */
    self->remote_seg_id   = answer.segment_id;
    self->ep_conn_index   = ep_conn_index;
    self->ep_conn_seq_num  = 1;
    self->dma_outstanding  = 0;
    self->gets_outstanding = 0;
    ucs_arbiter_group_init(&self->pending_q);

    /* --- Connect to the assigned window in the remote receive segment (AM sends) --- */
    ret = uct_pcie_connect_segment(
        iface->vdev_ep,
        answer.offset,
        iface->packet_size_bytes * iface->packet_queue_len,
        self->remote_node_id,
        self->remote_seg_id,
        &self->remote_segment,
        &self->remote_seg_map,
        (volatile void **)&self->remote_seg_buf,
        uct_pcie_ep_remote_segment_cb,
        self);
    if (ret != 0) {
        ucs_error("EP: failed to connect to remote receive segment %u on node %u",
                  self->remote_seg_id, self->remote_node_id);
        iface->eps_init_cnt--;
        return UCS_ERR_NO_RESOURCE;
    }
    self->is_connected = 1;

    ucs_debug("EP connected to segment %d at node %d\n",
              self->remote_seg_id, self->remote_node_id);

    return UCS_OK;
}

/* Discard callback for use when purging the pending queue on EP teardown.
 * UCX's contract requires the user to call ep_pending_purge before destroying
 * an EP, so this should never actually fire. It is here purely as a safety net. */
static ucs_arbiter_cb_result_t
uct_pcie_ep_pending_discard_cb(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                               ucs_arbiter_elem_t *elem, void *arg)
{
    ucs_warn("pcie EP destroyed with pending request still queued");
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}


/**
 * @brief Destroy endpoint and release its SISCI connections.
 *
 * self is a uct_pcie_ep_t *   - injected by the UCS_CLASS_CLEANUP_FUNC macro.
 */
static UCS_CLASS_CLEANUP_FUNC(uct_pcie_ep_t)
{
    uct_pcie_iface_t *iface = ucs_derived_of(self->super.super.iface, uct_pcie_iface_t);

    /* UCX guarantees ep_pending_purge is called before ep_destroy, so the
     * group should already be empty here. Drain defensively just in case. */
    ucs_arbiter_group_purge(
        &iface->arbiter,
        &self->pending_q,
        uct_pcie_ep_pending_discard_cb,
        NULL);

    /* UCP flushes an EP before destroying it; a transfer still in flight
     * would complete into a freed EP, so finish it here. */
    if (self->gets_outstanding != 0) {
        uct_pcie_iface_progress_gets(iface);
    }
    if (self->dma_outstanding != 0) {
        ucs_warn("pcie EP destroyed with %u DMA transfers in flight",
                 self->dma_outstanding);
        uct_pcie_ep_cancel_dma(iface, self);
    }

    /* Disconnect the AM receive segment established in init.
     * Null the AM write pointer first so any stray access is caught cleanly. */
    self->remote_seg_buf = NULL;
    uct_pcie_disconnect_segment(self->remote_segment, self->remote_seg_map);

    ucs_debug("EP destroyed: segment_id=%d node_id=%d\n",
              self->remote_seg_id, self->remote_node_id);
}


UCS_CLASS_DEFINE(uct_pcie_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NEW_FUNC(uct_pcie_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_pcie_ep_t, uct_ep_t);

/******************************************************************************/
/*** Put / Get (one-sided) ****************************************************/

/*
 * [UCT_REMOTE_ADDR] remote_addr is an address inside the range the peer
 * registered (uct_pcie_memh_t), and rkey is the connected remote segment it
 * was unpacked to (uct_pcie_rseg_t). The offset into that segment is
 * remote_addr - rseg->base; rseg->va + offset is the CPU-store window onto it.
 */

/* rkeys of same-node memory are not connected (see uct_pcie_rseg_same_node) */
#define UCT_PCIE_CHECK_RKEY(_rkey) \
    do { \
        if (ucs_unlikely(((uct_pcie_rseg_t*)(_rkey))->va == NULL)) { \
            return UCS_ERR_UNREACHABLE; \
        } \
    } while (0)

static UCS_F_ALWAYS_INLINE volatile void *
uct_pcie_ep_rma_va(uct_rkey_t rkey, uint64_t remote_addr, size_t length)
{
    uct_pcie_rseg_t *rseg = (uct_pcie_rseg_t*)rkey;

    ucs_assertv((remote_addr >= rseg->base) &&
                (remote_addr + length <= rseg->base + rseg->length),
                "remote_addr 0x%"PRIx64" length %zu outside registered range "
                "0x%"PRIx64"+%"PRIu64, remote_addr, length, rseg->base,
                rseg->length);
    return rseg->va + (remote_addr - rseg->base);
}

/**
 * @brief Write a small buffer into remote registered memory with CPU stores.
 */
ucs_status_t uct_pcie_ep_put_short(
    uct_ep_h tl_ep,
    const void *buffer,
    unsigned length,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    UCT_CHECK_LENGTH(length, 0, UCT_PCIE_MAX_PUT_SHORT, "put_short");
    UCT_PCIE_CHECK_RKEY(rkey);

    memcpy((void*)uct_pcie_ep_rma_va(rkey, remote_addr, length), buffer,
           length);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return UCS_OK;
}

/**
 * @brief Pack data straight into remote registered memory (CPU stores).
 *
 * @return Bytes written.
 */
ssize_t uct_pcie_ep_put_bcopy(
    uct_ep_h tl_ep,
    uct_pack_callback_t pack_cb,
    void *arg,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    ssize_t length;

    UCT_PCIE_CHECK_RKEY(rkey);
    length = pack_cb((void*)uct_pcie_ep_rma_va(rkey, remote_addr, 0), arg);
    ucs_assert(uct_pcie_ep_rma_va(rkey, remote_addr, length) != NULL);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return length;
}

/* Offset of a local buffer relative to the remote address, modulo the DMA
 * alignment: 0 if the DMA engine can run at full speed between them */
static UCS_F_ALWAYS_INLINE unsigned
uct_pcie_ep_dma_misalign(const void *buffer, uint64_t remote_addr)
{
    return ((uintptr_t)buffer ^ remote_addr) & (UCT_PCIE_DMA_ALIGN - 1);
}

/* Register the iface's staging buffer for misaligned gets (on first use) */
static ucs_status_t uct_pcie_iface_bounce_init(uct_pcie_iface_t *iface)
{
    size_t       size = iface->num_dmas * iface->bounce_slot;
    void        *buf;
    ucs_status_t status;

    buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0);
    if (buf == MAP_FAILED) {
        ucs_error("pcie: mmap(%zu) for the get staging buffer failed: %m", size);
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_md_mem_reg(iface->super.md, buf, size, UCT_MD_MEM_ACCESS_ALL,
                            &iface->bounce_memh);
    if (status != UCS_OK) {
        munmap(buf, size);
        return status;
    }

    iface->bounce_buf = buf;
    return UCS_OK;
}

/**
 * @brief Start a DMA transfer between a registered local buffer and remote
 *        registered memory on a free queue of the iface pool.
 *
 * Completion is reported from iface progress (uct_pcie_iface_progress_dma).
 * With bounce set (gets only), the transfer goes to the queue's slot of the
 * staging buffer at the remote address's offset modulo UCT_PCIE_DMA_ALIGN,
 * and progress copies it to iov->buffer when it is done.
 *
 * @param[in] flags  SCI_FLAG_DMA_READ for get, UCT_PCIE_NO_FLAGS for put.
 *
 * @return UCS_INPROGRESS, or UCS_ERR_NO_RESOURCE if all queues are busy.
 */
static ucs_status_t
uct_pcie_ep_dma(uct_pcie_ep_t *ep, const uct_iov_t *iov, uint64_t remote_addr,
                uct_rkey_t rkey, uct_completion_t *comp, unsigned flags,
                int bounce)
{
    uct_pcie_iface_t   *iface  = ucs_derived_of(ep->super.super.iface,
                                                uct_pcie_iface_t);
    uct_pcie_rseg_t    *rseg   = (uct_pcie_rseg_t*)rkey;
    uct_pcie_memh_t    *memh   = iov->memh;
    size_t              length = iov->length;
    sci_local_segment_t lseg;
    size_t              loff;
    uct_pcie_dma_t     *dma;
    unsigned int        index;
    sci_error_t         sci_error;
    ucs_status_t        status;

    UCT_PCIE_CHECK_RKEY(rkey);
    if (ucs_unlikely(iface->num_free_dmas == 0)) {
        return UCS_ERR_NO_RESOURCE;
    }

    ucs_assert(iov->count == 1);
    ucs_assert(uct_pcie_ep_rma_va(rkey, remote_addr, length) != NULL);

    if (bounce && ucs_unlikely(iface->bounce_buf == NULL)) {
        status = uct_pcie_iface_bounce_init(iface);
        if (status != UCS_OK) {
            return status;
        }
    }

    index = iface->free_dmas[--iface->num_free_dmas];
    dma   = &iface->dmas[index];
    if (bounce) {
        ucs_assert(length <= iface->bounce_slot - UCT_PCIE_DMA_ALIGN);
        memh          = iface->bounce_memh;
        loff          = (uintptr_t)iface->bounce_buf - memh->base +
                        index * iface->bounce_slot +
                        (remote_addr & (UCT_PCIE_DMA_ALIGN - 1));
        dma->copy_dst = iov->buffer;
        dma->copy_src = UCS_PTR_BYTE_OFFSET(memh->base, loff);
        dma->copy_len = length;
    } else {
        ucs_assert(memh != UCT_MEM_HANDLE_NULL);
        ucs_assert(((uintptr_t)iov->buffer >= memh->base) &&
                   ((uintptr_t)iov->buffer + length <=
                    memh->base + memh->length));
        loff = (uintptr_t)iov->buffer - memh->base;
    }
    lseg = memh->segment;

    SCIStartDmaTransfer(dma->queue, lseg, rseg->segment, loff, length,
                        remote_addr - rseg->base, UCT_PCIE_NO_CALLBACK, NULL,
                        flags, &sci_error);
    if (ucs_unlikely(sci_error != SCI_ERR_OK)) {
        dma->copy_len                            = 0;
        iface->free_dmas[iface->num_free_dmas++] = index;
        ucs_error("SCIStartDmaTransfer(%s, %zu bytes): %s",
                  (flags & SCI_FLAG_DMA_READ) ? "get" : "put", length,
                  SCIGetErrorString(sci_error));
        return UCS_ERR_IO_ERROR;
    }

    dma->comp = comp;
    dma->ep   = ep;
    ep->dma_outstanding++;
    return UCS_INPROGRESS;
}

/**
 * @brief Wait for and cancel the DMA transfers of an endpoint that goes away.
 */
void uct_pcie_ep_cancel_dma(uct_pcie_iface_t *iface, uct_pcie_ep_t *ep)
{
    sci_error_t   sci_error;
    unsigned int  i;

    for (i = 0; i < iface->num_dmas; i++) {
        if (iface->dmas[i].ep != ep) {
            continue;
        }
        SCIWaitForDMAQueue(iface->dmas[i].queue, SCI_INFINITE_TIMEOUT,
                           UCT_PCIE_NO_FLAGS, &sci_error);
        iface->dmas[i].ep                        = NULL;
        iface->dmas[i].copy_len                  = 0;
        iface->free_dmas[iface->num_free_dmas++] = i;
        ep->dma_outstanding--;
        if (iface->dmas[i].comp != NULL) {
            uct_invoke_completion(iface->dmas[i].comp, UCS_ERR_CANCELED);
        }
    }
}

/**
 * @brief Zero-copy put: DMA from the registered local buffer, or CPU stores
 *        for transfers up to put_pio_max (DMA costs ~7 us to start) and for
 *        buffers not aligned like the remote address (CPU stores 20.9 GB/s,
 *        misaligned DMA 4.5 GB/s, see UCT_PCIE_DMA_ALIGN).
 */
ucs_status_t uct_pcie_ep_put_zcopy(
    uct_ep_h tl_ep,
    const uct_iov_t *iov,
    size_t iovcnt,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    uct_pcie_ep_t    *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    size_t            total = uct_iov_total_length(iov, iovcnt);
    ucs_iov_iter_t    iov_iter;

    if ((total <= iface->put_pio_max) ||
        uct_pcie_ep_dma_misalign(iov->buffer, remote_addr)) {
        UCT_PCIE_CHECK_RKEY(rkey);
        ucs_iov_iter_init(&iov_iter);
        uct_iov_to_buffer(iov, iovcnt, &iov_iter,
                          (void*)uct_pcie_ep_rma_va(rkey, remote_addr, total),
                          total);
        SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
        return UCS_OK;
    }

    return uct_pcie_ep_dma(ep, iov, remote_addr, rkey, comp, UCT_PCIE_NO_FLAGS,
                           0);
}

/* Load the lines of a get into buf */
static UCS_F_ALWAYS_INLINE void
uct_pcie_iface_get_load(uct_pcie_iface_t *iface, const uct_pcie_get_req_t *req,
                        uint8_t *buf)
{
    iface->load_lines(buf, req->src,
                      ucs_div_round_up(req->head + req->length,
                                       UCT_PCIE_LINE_SIZE));
}

/**
 * @brief Complete the queued get_bcopy requests: first the CPU loads of all
 *        of them, so that their PCIe round trips overlap, then unpack each.
 *
 * One read of up to 1 KiB takes 1.3 us, but independent reads overlap
 * (0.4 us each for 64 bytes, tools/sisci_load_cost.c), so 64 gets in flight
 * no longer wait one round trip each. Gets issued from the callbacks run at
 * once (gets_busy), because the slots are still being read.
 *
 * @return Number of gets completed.
 */
unsigned uct_pcie_iface_progress_gets(uct_pcie_iface_t *iface)
{
    unsigned            count = iface->num_gets;
    uct_pcie_get_req_t *req;
    unsigned            i;

    if (count == 0) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        uct_pcie_iface_get_load(iface, &iface->gets[i],
                                iface->get_buf + i * iface->get_slot);
    }

    iface->gets_busy = 1;
    for (i = 0; i < count; i++) {
        req = &iface->gets[i];
        req->unpack_cb(req->arg,
                       iface->get_buf + i * iface->get_slot + req->head,
                       req->length);
        req->ep->gets_outstanding--;
        if (req->comp != NULL) {
            uct_invoke_completion(req->comp, UCS_OK);
        }
    }
    iface->num_gets  = 0;
    iface->gets_busy = 0;
    return count;
}

/**
 * @brief Small get with CPU loads: 64-byte streaming loads of the lines that
 *        hold the range, then unpack.
 *
 * Queued and completed by the next iface progress or flush, together with
 * the other queued gets (uct_pcie_iface_progress_gets); done at once if the
 * queue is full or being completed.
 */
ucs_status_t uct_pcie_ep_get_bcopy(
    uct_ep_h tl_ep,
    uct_unpack_callback_t unpack_cb,
    void *arg,
    size_t length,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    uct_pcie_ep_t          *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t       *iface = ucs_derived_of(tl_ep->iface,
                                                   uct_pcie_iface_t);
    const volatile uint8_t *src;
    uct_pcie_get_req_t     *req;
    uct_pcie_get_req_t      now;
    uint8_t                *buf;

    UCT_CHECK_LENGTH(length, 0, iface->get_pio_max, "get_bcopy");
    UCT_PCIE_CHECK_RKEY(rkey);

    src = uct_pcie_ep_rma_va(rkey, remote_addr, length);
    if (iface->gets_busy || (iface->num_gets == UCT_PCIE_GET_QUEUE)) {
        req = &now;
    } else {
        req = &iface->gets[iface->num_gets];
    }
    req->ep        = ep;
    req->unpack_cb = unpack_cb;
    req->arg       = arg;
    req->head      = (uintptr_t)src & (UCT_PCIE_LINE_SIZE - 1);
    req->src       = src - req->head;
    req->length    = length;
    req->comp      = comp;

    if (req == &now) {
        buf = iface->get_buf + UCT_PCIE_GET_QUEUE * iface->get_slot;
        uct_pcie_iface_get_load(iface, req, buf);
        unpack_cb(arg, buf + req->head, length);
        return UCS_OK;
    }

    iface->num_gets++;
    ep->gets_outstanding++;
    return UCS_INPROGRESS;
}

/**
 * @brief Zero-copy get: DMA read from remote registered memory into the
 *        registered local buffer, or into the staging buffer and copied from
 *        there if the local buffer is not aligned like the remote address
 *        (misaligned DMA runs at 4.5 instead of 27.4 GB/s; CPU loads from
 *        remote memory are slower still).
 */
ucs_status_t uct_pcie_ep_get_zcopy(
    uct_ep_h tl_ep,
    const uct_iov_t *iov,
    size_t iovcnt,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);

    if (uct_iov_total_length(iov, iovcnt) == 0) {
        return UCS_OK;
    }

    return uct_pcie_ep_dma(ep, iov, remote_addr, rkey, comp, SCI_FLAG_DMA_READ,
                           uct_pcie_ep_dma_misalign(iov->buffer, remote_addr));
}

/**
 * @brief Flush: wait for the endpoint's DMA transfers. CPU-store operations
 *        (AM, put short/bcopy) are complete when they return.
 */
ucs_status_t uct_pcie_ep_flush(
    uct_ep_h tl_ep,
    unsigned flags,
    uct_completion_t *comp)
{
    uct_pcie_ep_t        *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t     *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_flush_req_t *req;

    uct_pcie_iface_wc_flush(iface);
    if (ep->gets_outstanding != 0) {
        uct_pcie_iface_progress_gets(iface);
    }
    if (ep->dma_outstanding == 0) {
        UCT_TL_EP_STAT_FLUSH(&ep->super);
        return UCS_OK;
    }

    if (comp != NULL) {
        req = ucs_malloc(sizeof(*req), "uct_pcie_flush_req_t");
        if (req == NULL) {
            return UCS_ERR_NO_MEMORY;
        }
        req->comp = comp;
        req->ep   = ep;
        ucs_queue_push(&iface->flush_reqs, &req->queue);
    }

    UCT_TL_EP_STAT_FLUSH_WAIT(&ep->super);
    return UCS_INPROGRESS;
}

/*
 * [NO_ATOMICS] Atomic operations are not implemented in this transport and
 * will not be implemented in the foreseeable future.
 *
 * SISCI is a pure shared-memory-over-PCIe API: it provides segment creation,
 * remote mapping, and DMA transfer primitives. It exposes no facility for
 * atomic read-modify-write operations across the PCIe fabric.
 *
 * This transport therefore does not advertise any UCT_IFACE_FLAG_ATOMIC_*
 * capabilities. UCX's upper layers (UCP) detect the absence of those flags and
 * fall back to their own software emulation: atomic operations are carried out
 * via active messages routed through whichever transport supports AM, with the
 * target side performing the operation locally using CPU atomics and returning
 * the result. That path is correct, well-tested, and not meaningfully slower
 * than anything we could build here. The stubs below exist only to satisfy the
 * iface_ops vtable; they are never called in practice.
 */

ucs_status_t uct_pcie_ep_atomic32_post(
    uct_ep_h ep,
    unsigned opcode,
    uint32_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic64_post(
    uct_ep_h ep,
    unsigned opcode,
    uint64_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic64_fetch(
    uct_ep_h ep,
    uct_atomic_op_t opcode,
    uint64_t value,
    uint64_t *result,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic32_fetch(
    uct_ep_h ep,
    uct_atomic_op_t opcode,
    uint32_t value,
    uint32_t *result,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic_cswap64(
    uct_ep_h tl_ep,
    uint64_t compare,
    uint64_t swap,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uint64_t *result,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic_cswap32(
    uct_ep_h tl_ep,
    uint32_t compare,
    uint32_t swap,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uint32_t *result,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

/******************************************************************************/
/*** Active messages **********************************************************/

/*
 * Slot layout (see uct_pcie_am_hdr_t): the 64-byte header line, then the
 * payload at slot + 64 unless it fits into the header line (flag INLINE).
 * Payloads come from up to three sources in this order: a header (am_short's
 * uint64_t, am_zcopy's header), a contiguous buffer, an iov.
 */
typedef struct {
    const void      *hdr;
    size_t           hdr_len;
    const void      *buf;
    size_t           buf_len;
    const uct_iov_t *iov;
    size_t           iovcnt;
    size_t           iov_len;
} uct_pcie_am_src_t;

/* Writes a byte stream to the remote ring in whole 64-byte lines: full lines
 * straight from the source, partial ones assembled in line first. The last
 * line is padded; the receiver only reads am_length bytes. */
typedef struct {
    uint8_t           line[UCT_PCIE_LINE_SIZE];
    uct_pcie_iface_t *iface;
    volatile uint8_t *dst;
    size_t            fill;
} UCS_V_ALIGNED(UCT_PCIE_LINE_SIZE) uct_pcie_wc_writer_t;

static UCS_F_ALWAYS_INLINE void
uct_pcie_wc_write(uct_pcie_wc_writer_t *w, const void *src, size_t len)
{
    size_t n, lines;

    if (w->fill > 0) {
        n = ucs_min(len, UCT_PCIE_LINE_SIZE - w->fill);
        memcpy(w->line + w->fill, src, n);
        w->fill += n;
        src      = UCS_PTR_BYTE_OFFSET(src, n);
        len     -= n;
        if (w->fill < UCT_PCIE_LINE_SIZE) {
            return;
        }
        w->iface->store_lines(w->dst, w->line, 1);
        w->dst += UCT_PCIE_LINE_SIZE;
        w->fill = 0;
    }

    lines = len / UCT_PCIE_LINE_SIZE;
    if (lines > 0) {
        w->iface->store_lines(w->dst, src, lines);
        w->dst += lines * UCT_PCIE_LINE_SIZE;
        src     = UCS_PTR_BYTE_OFFSET(src, lines * UCT_PCIE_LINE_SIZE);
        len    -= lines * UCT_PCIE_LINE_SIZE;
    }

    if (len > 0) {
        memcpy(w->line, src, len);
        w->fill = len;
    }
}

static UCS_F_ALWAYS_INLINE void uct_pcie_wc_finish(uct_pcie_wc_writer_t *w)
{
    if (w->fill > 0) {
        w->iface->store_lines(w->dst, w->line, 1);
    }
}

/* Copy the payload into the header line (dst local, inline payload) */
static UCS_F_ALWAYS_INLINE void
uct_pcie_am_src_copy(void *dst, const uct_pcie_am_src_t *src)
{
    ucs_iov_iter_t iov_iter;

    memcpy(dst, src->hdr, src->hdr_len);
    dst = UCS_PTR_BYTE_OFFSET(dst, src->hdr_len);
    memcpy(dst, src->buf, src->buf_len);
    dst = UCS_PTR_BYTE_OFFSET(dst, src->buf_len);
    if (src->iovcnt > 0) {
        ucs_iov_iter_init(&iov_iter);
        uct_iov_to_buffer(src->iov, src->iovcnt, &iov_iter, dst, src->iov_len);
    }
}

/* Copy the payload behind the header line of a remote slot */
static UCS_F_ALWAYS_INLINE void
uct_pcie_am_src_write(uct_pcie_iface_t *iface, volatile uint8_t *dst,
                      const uct_pcie_am_src_t *src)
{
    uct_pcie_wc_writer_t w;
    size_t               i, k;

    w.iface = iface;
    w.dst   = dst;
    w.fill  = 0;
    uct_pcie_wc_write(&w, src->hdr, src->hdr_len);
    uct_pcie_wc_write(&w, src->buf, src->buf_len);
    for (i = 0; i < src->iovcnt; i++) {
        for (k = 0; k < src->iov[i].count; k++) {
            uct_pcie_wc_write(&w, UCS_PTR_BYTE_OFFSET(src->iov[i].buffer,
                                                      k * src->iov[i].stride),
                              src->iov[i].length);
        }
    }
    uct_pcie_wc_finish(&w);
}

/* Room in the ring: sequence numbers not yet acked by the receiver */
static UCS_F_ALWAYS_INLINE int
uct_pcie_ep_am_has_room(uct_pcie_ep_t *ep, uct_pcie_iface_t *iface)
{
    return ep->ep_conn_seq_num - iface->ctls[ep->ep_conn_index].ep_conn_ack <=
           iface->packet_queue_len;
}

static UCS_F_ALWAYS_INLINE volatile uint8_t *
uct_pcie_ep_am_slot(uct_pcie_ep_t *ep, uct_pcie_iface_t *iface)
{
    return &ep->remote_seg_buf[iface->packet_size_bytes *
                               (ep->ep_conn_seq_num % iface->packet_queue_len)];
}

/* Write the header line of the current slot and advance the sequence number.
 * Any payload beyond the line must have been written and fenced already. */
static UCS_F_ALWAYS_INLINE void
uct_pcie_ep_am_post(uct_pcie_ep_t *ep, uct_pcie_iface_t *iface,
                    volatile uint8_t *slot, uct_pcie_am_hdr_t *line, uint8_t id,
                    size_t length)
{
    line->am_id     = id;
    line->am_length = length;
    line->seq       = ep->ep_conn_seq_num++;
    iface->store_line(slot, line);
    iface->wc_dirty = 1;
}

/* Fill the header line's data and write the continuation lines, every one
 * carrying the message's seq (see UCT_PCIE_AM_FLAG_LINES) */
static UCS_F_ALWAYS_INLINE void
uct_pcie_ep_am_write_lines(uct_pcie_ep_t *ep, uct_pcie_iface_t *iface,
                           volatile uint8_t *slot, uct_pcie_am_hdr_t *line,
                           const uct_pcie_am_src_t *src, size_t length)
{
    const uint8_t *payload = iface->lines_tx_buf;
    size_t         rest    = length - UCT_PCIE_AM_INLINE_MAX;
    size_t         count   = ucs_div_round_up(rest, UCT_PCIE_AM_LINE_DATA);
    size_t         i;

    uct_pcie_am_src_copy(iface->lines_tx_buf, src);
    memcpy(line->data, payload, UCT_PCIE_AM_INLINE_MAX);
    payload += UCT_PCIE_AM_INLINE_MAX;
    for (i = 0; i < count; i++) {
        memcpy(iface->lines_tx[i].data, payload,
               ucs_min(rest, UCT_PCIE_AM_LINE_DATA));
        iface->lines_tx[i].seq = ep->ep_conn_seq_num;
        payload               += UCT_PCIE_AM_LINE_DATA;
        rest                  -= ucs_min(rest, UCT_PCIE_AM_LINE_DATA);
    }
    iface->store_lines(slot + sizeof(*line), iface->lines_tx, count);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_pcie_ep_am_send(uct_pcie_ep_t *ep, uint8_t id, const uct_pcie_am_src_t *src)
{
    uct_pcie_iface_t  *iface  = ucs_derived_of(ep->super.super.iface,
                                               uct_pcie_iface_t);
    size_t             length = src->hdr_len + src->buf_len + src->iov_len;
    uct_pcie_am_hdr_t  line;
    volatile uint8_t  *slot;

    /* Flow control: returning UCS_ERR_NO_RESOURCE makes UCX queue the send
     * with ep_pending_add; iface progress retries it once acks arrive. The
     * receiver can only ack what has left our write-combining buffers. */
    if (!uct_pcie_ep_am_has_room(ep, iface)) {
        uct_pcie_iface_wc_flush(iface);
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_CHECK_LENGTH(length, 0,
                     iface->packet_size_bytes - sizeof(uct_pcie_am_hdr_t), "am");
    UCT_CHECK_AM_ID(id);

    slot = uct_pcie_ep_am_slot(ep, iface);
    if (length <= UCT_PCIE_AM_INLINE_MAX) {
        line.am_flags = UCT_PCIE_AM_FLAG_INLINE;
        uct_pcie_am_src_copy(line.data, src);
    } else if (length <= iface->am_lines_payload_max) {
        line.am_flags = UCT_PCIE_AM_FLAG_LINES;
        uct_pcie_ep_am_write_lines(ep, iface, slot, &line, src, length);
    } else {
        line.am_flags = 0;
        uct_pcie_am_src_write(iface, slot + sizeof(line), src);
        SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    }

    uct_pcie_ep_am_post(ep, iface, slot, &line, id, length);
    return UCS_OK;
}

/**
 * @brief Send a "short" active message: uint64_t header, then the payload.
 *
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE if the ring is full.
 */
ucs_status_t uct_pcie_ep_am_short(
    uct_ep_h tl_ep,
    uint8_t id,
    uint64_t header,
    const void *payload,
    unsigned length)
{
    uct_pcie_ep_t     *ep  = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_am_src_t  src = {
        .hdr = &header, .hdr_len = sizeof(header),
        .buf = payload, .buf_len = length
    };

    return uct_pcie_ep_am_send(ep, id, &src);
}

/**
 * @brief Send a short active message gathered from an iov.
 *
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE if the ring is full.
 */
ucs_status_t uct_pcie_ep_am_short_iov(
    uct_ep_h tl_ep,
    uint8_t id,
    const uct_iov_t *iov,
    size_t iovcnt)
{
    uct_pcie_ep_t     *ep  = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_am_src_t  src = {
        .iov = iov, .iovcnt = iovcnt,
        .iov_len = uct_iov_total_length(iov, iovcnt)
    };

    return uct_pcie_ep_am_send(ep, id, &src);
}

/**
 * @brief Send an active message packed by a caller callback (bcopy).
 *
 * pack_cb writes into a local staging buffer (it uses memcpy, which is slow
 * into the PCIe mapping); the result then goes out like any other payload.
 *
 * @return Bytes sent, or UCS_ERR_NO_RESOURCE if the ring is full.
 */
ssize_t uct_pcie_ep_am_bcopy(
    uct_ep_h tl_ep,
    uint8_t id,
    uct_pack_callback_t pack_cb,
    void *arg,
    unsigned flags)
{
    uct_pcie_ep_t     *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t  *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_am_src_t  src   = {};
    ucs_status_t       status;

    if (!uct_pcie_ep_am_has_room(ep, iface)) {
        return UCS_ERR_NO_RESOURCE;
    }

    src.buf     = iface->bcopy_buf;
    src.buf_len = pack_cb(iface->bcopy_buf, arg);
    status      = uct_pcie_ep_am_send(ep, id, &src);
    return (status == UCS_OK) ? (ssize_t)src.buf_len : status;
}

/**
 * @brief Send an active message from a header and an iov. The data is copied
 *        with CPU stores, so the operation is complete when it returns.
 *
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE if the ring is full.
 */
ucs_status_t uct_pcie_ep_am_zcopy(
    uct_ep_h uct_ep,
    uint8_t id,
    const void *header,
    unsigned header_length,
    const uct_iov_t *iov,
    size_t iovcnt,
    unsigned flags,
    uct_completion_t *comp)
{
    uct_pcie_ep_t     *ep  = ucs_derived_of(uct_ep, uct_pcie_ep_t);
    uct_pcie_am_src_t  src = {
        .hdr = header, .hdr_len = header_length,
        .iov = iov, .iovcnt = iovcnt,
        .iov_len = uct_iov_total_length(iov, iovcnt)
    };

    return uct_pcie_ep_am_send(ep, id, &src);
}

/******************************************************************************/
/*** Pending queue ************************************************************/

/*
 * When the flow-control window is full, send functions return
 * UCS_ERR_NO_RESOURCE and UCX calls ep_pending_add to queue the request.
 * iface_progress retries queued requests after draining incoming packets,
 * once ack counters have advanced and slots have opened up.
 */

typedef struct {
    uct_pending_purge_callback_t cb;
    void                        *arg;
} uct_pcie_pending_purge_args_t;

/* Callback used by pending_purge: invokes the caller-supplied cancel function
 * for each element then removes it from the arbiter. */
static ucs_arbiter_cb_result_t
uct_pcie_ep_pending_purge_cb(
    ucs_arbiter_t       *arbiter,
    ucs_arbiter_group_t *group,
    ucs_arbiter_elem_t  *elem,
    void                *arg)
{
    uct_pcie_pending_purge_args_t *args = arg;
    uct_pending_req_t             *req  = ucs_container_of(elem, uct_pending_req_t, priv);

    if (args->cb != NULL) {
        args->cb(req, args->arg);
    }
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

/**
 * @brief Add a send request to the per-EP pending queue.
 *
 * Called by UCX when an AM send returns UCS_ERR_NO_RESOURCE. Queues the
 * request in the EP's arbiter group for retry from iface_progress.
 *
 * @return UCS_OK       request was queued
 * @return UCS_ERR_BUSY the send window has room and no earlier requests are
 *                      waiting, so queuing is unnecessary. Despite the
 *                      misleading name, UCS_ERR_BUSY in this context is a
 *                      UCX API convention meaning "the send window is open,
 *                      skip the queue and retry the send directly."
 */
ucs_status_t uct_pcie_ep_pending_add(
    uct_ep_h tl_ep,
    uct_pending_req_t *req,
    unsigned flags)
{
    uct_pcie_ep_t    *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);

    /* If the window has room and no prior requests are waiting (which would
     * need to go first to preserve ordering), tell UCX to retry directly. */
    if (ucs_arbiter_group_is_empty(&ep->pending_q) &&
        uct_pcie_ep_am_has_room(ep, iface) &&
        (iface->num_free_dmas > 0)) {
        return UCS_ERR_BUSY;
    }

    /* uct_pending_req_arb_group_push stores the arbiter element in req->priv
     * (UCT-reserved space) and appends it to this EP's arbiter group. */
    uct_pending_req_arb_group_push(&ep->pending_q, req);

    /* Tell the iface arbiter to include this group in the next dispatch.
     * Idempotent: safe to call even if the group is already scheduled. */
    ucs_arbiter_group_schedule(&iface->arbiter, &ep->pending_q);

    return UCS_OK;
}

/**
 * @brief Cancel all pending requests for an EP.
 *
 * Drains the EP's arbiter group and invokes the caller-supplied callback for
 * each request so the upper layer can clean up (e.g. free the request).
 * Typically called during EP teardown or MPI error recovery.
 */
void uct_pcie_ep_pending_purge(
    uct_ep_h tl_ep,
    uct_pending_purge_callback_t cb,
    void *arg)
{
    uct_pcie_ep_t                 *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t              *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_pending_purge_args_t  args  = {.cb = cb, .arg = arg};

    ucs_arbiter_group_purge(&iface->arbiter, &ep->pending_q,
                            uct_pcie_ep_pending_purge_cb,
                            &args);
}


