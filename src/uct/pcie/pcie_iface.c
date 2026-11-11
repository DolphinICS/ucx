
#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>

#include <sys/mman.h>

#include "stdio.h"

#include "pcie_md.h"
#include "pcie_iface.h"
#include "pcie_ep.h"
#include "pcie_sisci_helper.h"

/* Forward declarations */
static uct_iface_ops_t uct_pcie_iface_ops;

#if defined(__x86_64__)
#include <immintrin.h>

/* One 64-byte non-temporal store fills a whole write-combining buffer, which
 * goes out as one PCIe write: the line arrives in one piece, no fence needed. */
__attribute__((target("avx512f"))) static void
uct_pcie_store_line_avx512(volatile void *dst, const void *src)
{
    _mm512_stream_si512((__m512i*)dst, _mm512_load_si512(src));
}

/* Streaming loads (vmovntdqa) from the write-combining mapping of remote
 * memory overlap: one read of up to 1 KiB took 1.3-1.4 us, 4 KiB 3.9 us
 * (tools/sisci_load_cost.c), against ~4.7 us for the smallest DMA get */
__attribute__((target("avx512f"))) static void
uct_pcie_load_lines_avx512(void *dst, const volatile void *src, size_t lines)
{
    __m512i *d = dst;
    size_t   i;

    for (i = 0; i < lines; i++) {
        _mm512_store_si512(d + i,
                           _mm512_stream_load_si512((__m512i*)src + i));
    }
}

__attribute__((target("avx512f"))) static void
uct_pcie_store_lines_avx512(volatile void *dst, const void *src, size_t lines)
{
    __m512i       *d = (__m512i*)dst;
    const uint8_t *s = src;
    size_t         i;

    for (i = 0; i < lines; i++) {
        _mm512_stream_si512(d + i, _mm512_loadu_si512(s + i * UCT_PCIE_LINE_SIZE));
    }
}
#endif

static void
uct_pcie_store_lines_memcpy(volatile void *dst, const void *src, size_t lines)
{
    memcpy((void*)dst, src, lines * UCT_PCIE_LINE_SIZE);
}

/* Without a single 64-byte store: the first 56 bytes, fence, the last 8 */
static void uct_pcie_store_line_fenced(volatile void *dst, const void *src)
{
    memcpy((void*)dst, src, UCT_PCIE_LINE_SIZE - sizeof(uint64_t));
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    *(volatile uint64_t*)UCS_PTR_BYTE_OFFSET(dst, UCT_PCIE_LINE_SIZE -
                                             sizeof(uint64_t)) =
        *(const uint64_t*)UCS_PTR_BYTE_OFFSET(src, UCT_PCIE_LINE_SIZE -
                                              sizeof(uint64_t));
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
}

static ucs_config_field_t uct_pcie_iface_config_table[] = {
    /* MAX_NUM_EPS overrides the UCX base iface config. Reported via
     * iface_attr.max_num_eps; advisory only; the UCT framework does not gate
     * ep_create calls against it. Keep in sync with PCIE_MAX_EPS below and
     * UCT_PCIE_MAX_EPS in pcie_iface.h. */
    {"", "MAX_NUM_EPS=32", NULL,
     ucs_offsetof(uct_pcie_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"SEND_SIZE", "64k",
     "Size of one AM ring slot (header line + payload). Larger messages go "
     "through rendezvous, i.e. DMA",
     ucs_offsetof(uct_pcie_iface_config_t, packet_size_bytes),
        UCS_CONFIG_TYPE_MEMUNITS},

    {
        /* Runtime SISCI slot count. Must not exceed UCT_PCIE_MAX_EPS (the
         * compile-time array bound). Keep in sync with MAX_NUM_EPS above. */
        "MAX_EPS", "32", "Maximum number of simultaneous endpoint connections",
        ucs_offsetof(uct_pcie_iface_config_t, max_eps),
        UCS_CONFIG_TYPE_UINT
    },

    {
        "PACKET_QUEUE_LEN", "64", "AM ring slots per connection. With 5 slots "
        "a stream of 8-byte messages cost 0.38 us each (waiting for acks), "
        "with 64 slots 0.05 us",
        ucs_offsetof(uct_pcie_iface_config_t, packet_queue_len),
        UCS_CONFIG_TYPE_UINT
    },

    {"DMA_QUEUES", "8",
     "Number of DMA queues, i.e. put/get zero-copy transfers in flight per "
     "interface (at most " UCS_PP_MAKE_STRING(UCT_PCIE_MAX_DMA_QUEUES) ")",
     ucs_offsetof(uct_pcie_iface_config_t, dma_queues),
     UCS_CONFIG_TYPE_UINT},

    {"PUT_PIO_MAX", "32k",
     "put_zcopy up to this size is written with CPU stores through the PCIe "
     "mapping instead of DMA",
     ucs_offsetof(uct_pcie_iface_config_t, put_pio_max),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"GET_PIO_MAX", "2k",
     "Gets up to this size are read with CPU loads through the PCIe mapping\n"
     "(get_bcopy) instead of DMA: 64-byte streaming loads read up to 1 KiB in\n"
     "1.3-1.4 us, 4 KiB in 3.9 us; the smallest DMA get takes ~4.7 us. Many\n"
     "gets in flight overlap their loads, but from 256 B on DMA streams them\n"
     "faster (MPI_Get of 1 KiB, 64 in flight: 0.69 vs 0.21 us each). Needs\n"
     "AVX-512, otherwise off.",
     ucs_offsetof(uct_pcie_iface_config_t, get_pio_max),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"AM_LINES", "16",
     "Most 64-byte lines of an active message sent without a fence, every "
     "line carrying a sequence number (at most "
     UCS_PP_MAKE_STRING(UCT_PCIE_AM_LINES_MAX) ", 1 = header line only). "
     "MPI message streams, us each, 4 vs. 16 lines: 256 B 0.257 vs 0.169, "
     "512 B 0.270 vs 0.194, 1 KiB and more unchanged (gathering the lines "
     "costs as much as the fence); 32 lines made 1 KiB both ways slower.",
     ucs_offsetof(uct_pcie_iface_config_t, am_lines),
     UCS_CONFIG_TYPE_UINT},

    {"DMA_OVERHEAD", "3us",
     "DMA engine time per put/get zero-copy transfer beyond the data time, "
     "as reported to UCP for choosing between CPU-store eager sends and "
     "DMA rendezvous. 3 us puts the switch for message streams at ~25 KiB: "
     "16 KiB eager 0.86 us per message (rendezvous 1.86), 32 KiB both ways "
     "rendezvous 2.16 us (eager 2.99)",
     ucs_offsetof(uct_pcie_iface_config_t, dma_overhead),
     UCS_CONFIG_TYPE_TIME},

    {"ZCOPY_MAX", "1m",
     "Largest single put/get zero-copy transfer; larger messages are split "
     "by UCP and their pieces run on several DMA queues at once",
     ucs_offsetof(uct_pcie_iface_config_t, zcopy_max),
     UCS_CONFIG_TYPE_MEMUNITS},

    {NULL}
};

/******************************************************************************/
/*** Connection handling ******************************************************/

/* Bytes of one connection's AM receive segment: the ring, in whole pages
 * (registered memory) */
static size_t uct_pcie_iface_ring_size(const uct_pcie_iface_t *iface)
{
    return ucs_align_up_pow2(iface->packet_size_bytes * iface->packet_queue_len,
                             ucs_get_page_size());
}

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
    const uct_pcie_conn_req_t *request,
    unsigned int *cd_index)
{
    unsigned int i;
    int rc = 0;

    pthread_mutex_lock(&iface->lock);

    /* A repeated request (the answer was late or lost, see
     * uct_pcie_ep_send_recv_conn_request) */
    for (i = 0; i < iface->max_eps; i++) {
        if ((iface->sci_cds[i].cd_status != UCT_PCIE_CD_AVAILABLE) &&
            (iface->sci_cds[i].remote_node == request->node_id) &&
            (iface->sci_cds[i].remote_ctl_segment_id == request->ctl_segment_id) &&
            (iface->sci_cds[i].remote_ep_conn_index == request->ep_conn_index)) {
            *cd_index = i;
            pthread_mutex_unlock(&iface->lock);
            return 1;
        }
    }

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
        iface->sci_cds[i].cd_status             = UCT_PCIE_CD_RESERVED;
        iface->sci_cds[i].remote_node           = request->node_id;
        iface->sci_cds[i].remote_ctl_segment_id = request->ctl_segment_id;
        iface->sci_cds[i].remote_ep_conn_index  = request->ep_conn_index;
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
    unsigned int attempt;

    /* The requester retries if no answer comes (it may have given up) */
    for (attempt = 0; attempt < UCT_PCIE_CONN_ATTEMPTS; attempt++) {
        SCIConnectDataInterrupt(
            sci_virtual_device,
            &ans_interrupt,
            request->node_id,
            UCT_PCIE_LOCAL_ADAPTER_NO,
            request->interrupt,
            1000,
            UCT_PCIE_NO_FLAGS,
            &sci_error);
        if (sci_error == SCI_ERR_OK) {
            break;
        }
    }
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("pcie: cannot answer connection request from node %d: %s",
                 request->node_id, SCIGetErrorString(sci_error));
        return -1;
    }

    answer.segment_id = iface->recv_segment_id;
    answer.offset     = (uint64_t)(sci_cd->packet_queue_buf - iface->recv_buf);
    
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
 * @brief SISCI data interrupt callback fired when a remote endpoint connects.
 *
 * Reserves a connection slot (sci_cd), clears the slot's window of the receive
 * segment, sends segment ID and window offset to the connecting endpoint, then
 * connects to the remote control segment so the iface can write acknowledgement
 * counters into it during progress.
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

    ret = uct_pcie_reserve_control_descriptor(iface, request, &sci_cd_index);
    if (ret < 0) {
        ucs_error("Number of endpoints exceeds limit %u", iface->max_eps);
        return SCI_CALLBACK_CONTINUE;
    }
    sci_cd = &(iface->sci_cds[sci_cd_index]);
    if (ret > 0) {
        /* Already set up (or being set up): answer again with the same slot */
        if (sci_cd->cd_status == UCT_PCIE_CD_READY) {
            uct_pcie_send_answer_to_request(iface, request,
                                            md->sci_virtual_device, sci_cd);
        }
        return SCI_CALLBACK_CONTINUE;
    }

    /* The slot's window of the receive segment; a slot is not reused, but
     * clear it in case an earlier connection attempt failed after writing */
    sci_cd->packet_queue_buf = iface->recv_buf +
                               (size_t)sci_cd_index * uct_pcie_iface_ring_size(iface);
    memset(sci_cd->packet_queue_buf, 0, uct_pcie_iface_ring_size(iface));
    sci_cd->ep_conn_last_ack = 0;
    sci_cd->ep_conn_sent_ack = 0;

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
        (volatile void **)&sci_cd->ctl_buf,
        NULL, NULL);
    if (ret != 0) {
        ucs_error("Failed to connect to remote control buffer");
        uct_pcie_ureserve_control_descriptor(iface, sci_cd_index);
        return SCI_CALLBACK_CONTINUE;
    }

    sci_cd->cd_status = UCT_PCIE_CD_READY;
    return SCI_CALLBACK_CONTINUE;
}

/******************************************************************************/
/*** Reachability and connectivity ********************************************/

/**
 * @brief Probe whether a remote SISCI node is reachable.
 *
 * Called by UCX before creating an EP to decide whether a connection attempt
 * is worth making. When a device address is supplied, calls SCIProbeNode to
 * check whether the node is visible. Without an address there is nothing to
 * probe, so the function returns 1 and lets the connection attempt determine
 * reachability on its own.
 *
 * Returning 1 is not a promise. It is permission to try. If the attempt
 * fails, EP creation returns an error in the normal way.
 *
 * @param[in] tl_iface  Local interface handle.
 * @param[in] params    Query parameters; may carry a device address and an
 *                      info_string buffer for a human-readable failure reason.
 * @return 1 if the remote node is (likely) reachable, 0 otherwise.
 */
static int
uct_pcie_iface_is_reachable_v2(
    const uct_iface_h tl_iface,
    const uct_iface_is_reachable_params_t *params)
{
    uct_pcie_iface_t             *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_md_t                *md    = ucs_derived_of(iface->super.md, uct_pcie_md_t);
    const uct_pcie_device_addr_t *dev_addr;
    sci_error_t                   sci_error;
    int                           reachable;

    if (!(params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_DEVICE_ADDR)) {
        return 1; /* no address to probe; assume reachable */
    }

    dev_addr = (const uct_pcie_device_addr_t *)params->device_addr;

    /* Peers on the own node go through self/sm. Over the adapter in loopback,
     * registered segments cannot be mapped and AM traffic between two
     * processes of one node stalls, so UCP must not pick pcie for them. */
    if (dev_addr->node_id == md->node_id) {
        if ((params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING) &&
            (params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING_LENGTH)) {
            ucs_snprintf_zero(params->info_string, params->info_string_length,
                              "SISCI node %u is the local node", dev_addr->node_id);
        }
        return 0;
    }

    reachable = SCIProbeNode(md->sci_virtual_device,
                             UCT_PCIE_LOCAL_ADAPTER_NO,
                             dev_addr->node_id,
                             UCT_PCIE_NO_FLAGS,
                             &sci_error);

    if (!reachable &&
        (params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING) &&
        (params->field_mask & UCT_IFACE_IS_REACHABLE_FIELD_INFO_STRING_LENGTH)) {
        ucs_snprintf_zero(params->info_string, params->info_string_length,
                          "SISCI node %u is not reachable via PCIe adapter %u",
                          dev_addr->node_id, UCT_PCIE_LOCAL_ADAPTER_NO);
    }

    return reachable;
}

/**
 * @brief Report whether the endpoint's remote peer is still connected.
 *
 * Reads the volatile is_connected flag, which is set to 1 after EP creation
 * and cleared to 0 by uct_pcie_ep_remote_segment_cb when the remote segment
 * disappears. Returning 0 means that callback has fired.
 *
 * @param[in] tl_ep  Endpoint handle.
 * @param[in] params Query parameters (field_mask is not used here).
 * @return 1 if still connected, 0 if the remote peer has disconnected.
 */
int uct_pcie_ep_is_connected(
    const uct_ep_h tl_ep,
    const uct_ep_is_connected_params_t *params)
{
    const uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    return ep->is_connected;
}

/******************************************************************************/
/*** Iface lifecycle **********************************************************/

static void uct_pcie_iface_remove_dma_queues(uct_pcie_iface_t *iface)
{
    sci_error_t sci_error;
    unsigned int i;

    ucs_assertv(iface->num_free_dmas == iface->num_dmas,
                "%u DMA transfers still in flight",
                iface->num_dmas - iface->num_free_dmas);
    for (i = 0; i < iface->num_dmas; i++) {
        SCIRemoveDMAQueue(iface->dmas[i].queue, UCT_PCIE_NO_FLAGS, &sci_error);
        if (sci_error != SCI_ERR_OK) {
            ucs_error("SCIRemoveDMAQueue: %s", SCIGetErrorString(sci_error));
        }
    }
    iface->num_dmas      = 0;
    iface->num_free_dmas = 0;
}

/**
 * @brief Per-operation performance, measured fpga1614 <-> fpga1610 (MXH930).
 *
 * CPU stores (short/bcopy/AM) reach 20.4 GB/s from one core; DMA (put/get
 * zcopy) reaches 27 GB/s but costs ~7 us to start. Without this split UCP
 * would assume the same bandwidth for both and never prefer DMA.
 */
static ucs_status_t
uct_pcie_iface_estimate_perf(uct_iface_h tl_iface, uct_perf_attr_t *perf_attr)
{
    uct_pcie_iface_t  *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_ep_operation_t op    = UCT_ATTR_VALUE(PERF, perf_attr, operation,
                                              OPERATION, UCT_EP_OP_LAST);
    int                dma   = (op == UCT_EP_OP_GET_ZCOPY) ||
                               (op == UCT_EP_OP_PUT_ZCOPY);
    ucs_status_t       status;

    status = uct_base_iface_estimate_perf(tl_iface, perf_attr);
    if (status != UCS_OK) {
        return status;
    }

    if (op == UCT_EP_OP_GET_BCOPY) {
        /* Streaming CPU loads (see GET_PIO_MAX), one round trip each. The
         * bandwidth puts the crossing with the DMA estimate (which UCP adds
         * the registration cost to) at ~3 KiB, so that UCP does not split
         * larger gets into get_pio_max pieces:
         * with 64 gets in flight, loads took 2.7 us per 4 KiB get and DMA
         * 1.5 us (a single 4 KiB get: 3.0 and 5.7 us) */
        if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_BANDWIDTH) {
            perf_attr->bandwidth.dedicated = 0.25 * UCS_GBYTE;
            perf_attr->bandwidth.shared    = 0;
        }
        if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_LATENCY) {
            perf_attr->latency = ucs_linear_func_make(1.3e-6, 0);
        }
        return UCS_OK;
    }

    if (!dma) {
        return UCS_OK;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_BANDWIDTH) {
        perf_attr->bandwidth.dedicated = 27.0 * UCS_GBYTE;
        perf_attr->bandwidth.shared    = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_PATH_BANDWIDTH) {
        perf_attr->path_bandwidth.dedicated = 27.0 * UCS_GBYTE;
        perf_attr->path_bandwidth.shared    = 0;
    }

    /* Per-transfer DMA engine time (config DMA_OVERHEAD); UCP weighs it
     * against CPU-store eager sends, see the config help */
    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_PRE_OVERHEAD) {
        perf_attr->send_pre_overhead = iface->dma_overhead;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_LATENCY) {
        perf_attr->latency = ucs_linear_func_make(7e-6, 0);
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_MAX_INFLIGHT_EPS) {
        perf_attr->max_inflight_eps = iface->num_dmas;
    }

    return UCS_OK;
}

static uct_iface_internal_ops_t uct_base_iface_internal_ops = {
    .iface_estimate_perf   = uct_pcie_iface_estimate_perf,
    .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query              = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = (uct_ep_connect_to_ep_v2_func_t)ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_pcie_iface_is_reachable_v2,
    .ep_is_connected       = uct_pcie_ep_is_connected
};

/**
 * @brief Construct interface and set up internal resources pertaining to it.
 *
 * Creates the receive and control segments, initializes the connection
 * descriptor table, sets up the DMA engine (reserved for future use), and
 * installs the connection interrupt that accepts incoming EP connections.
 *
 * self is a uct_pcie_iface_t *   - injected by the UCS_CLASS_INIT_FUNC macro.
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
    uct_pcie_iface_config_t* config = ucs_derived_of(tl_config, uct_pcie_iface_config_t);
    uct_pcie_md_t* sci_md = ucs_derived_of(md, uct_pcie_md_t);

    size_t control_segment_size;

    /* --- Validate open parameters --- */
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

    /* --- Initialize synchronization primitives --- */
    if (pthread_mutex_init(&self->lock, NULL) != 0) {
        ucs_error("pthread_mutex_init failed");
        goto err;
    }

    /* Arbiter must be ready before any EP is created, since EPs schedule
     * their pending groups here. */
    ucs_arbiter_init(&self->arbiter);
    self->wc_dirty     = 0;
    self->bcopy_buf    = NULL;
    self->lines_tx_buf = NULL;
    self->lines_rx_buf = NULL;
    self->lines_tx     = NULL;

    UCS_CLASS_CALL_SUPER_INIT(
            uct_base_iface_t, &uct_pcie_iface_ops, &uct_base_iface_internal_ops,
            md, worker, params,
            tl_config UCS_STATS_ARG(
                    (params->field_mask & UCT_IFACE_PARAM_FIELD_STATS_ROOT) ?
                            params->stats_root :
                            NULL) UCS_STATS_ARG(UCT_PCIE_NAME));

    /* --- Query local SISCI node ID --- */
    SCIGetLocalNodeId(adapterID, &nodeID, flags, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIGetLocalNodeId: %s", SCIGetErrorString(sci_error));
        goto err_destroy_mutex;
    }

    /* uct_pcie_iface_t *self: store configuration */
    self->device_addr = nodeID;
    self->packet_size_bytes = ucs_align_up_pow2(config->packet_size_bytes,
                                                UCT_PCIE_LINE_SIZE);
    self->packet_queue_len = config->packet_queue_len;
    self->ack_batch        = ucs_max(1, config->packet_queue_len / 2);
    self->store_line       = uct_pcie_store_line_fenced;
    self->store_lines      = uct_pcie_store_lines_memcpy;
#if defined(__x86_64__)
    self->load_lines  = NULL;
    self->get_pio_max = 0;
    if (__builtin_cpu_supports("avx512f")) {
        self->store_line  = uct_pcie_store_line_avx512;
        self->store_lines = uct_pcie_store_lines_avx512;
        self->load_lines  = uct_pcie_load_lines_avx512;
        self->get_pio_max = config->get_pio_max;
    }
#endif
    self->bcopy_buf    = ucs_malloc(self->packet_size_bytes, "pcie bcopy");
    self->lines_tx_buf = ucs_malloc(UCT_PCIE_AM_LINES_PAYLOAD_MAX, "pcie lines tx");
    self->lines_rx_buf = ucs_malloc(UCT_PCIE_AM_LINES_PAYLOAD_MAX, "pcie lines rx");
    if (ucs_posix_memalign((void**)&self->lines_tx, UCT_PCIE_LINE_SIZE,
                           UCT_PCIE_AM_LINES_MAX * UCT_PCIE_LINE_SIZE,
                           "pcie lines") != 0) {
        self->lines_tx = NULL;
    }
    self->get_slot  = ucs_align_up_pow2(self->get_pio_max +
                                        2 * UCT_PCIE_LINE_SIZE,
                                        UCT_PCIE_LINE_SIZE);
    self->num_gets  = 0;
    self->gets_busy = 0;
    if (ucs_posix_memalign((void**)&self->get_buf, UCT_PCIE_LINE_SIZE,
                           (UCT_PCIE_GET_QUEUE + 1) * self->get_slot,
                           "pcie get") != 0) {
        self->get_buf = NULL;
    }
    if ((self->bcopy_buf == NULL) || (self->lines_tx_buf == NULL) ||
        (self->lines_rx_buf == NULL) || (self->lines_tx == NULL) ||
        (self->get_buf == NULL) ||
        (self->packet_size_bytes < UCT_PCIE_AM_LINES_MAX * UCT_PCIE_LINE_SIZE)) {
        ucs_error("pcie: SEND_SIZE must be at least %d bytes",
                  UCT_PCIE_AM_LINES_MAX * UCT_PCIE_LINE_SIZE);
        goto err_destroy_mutex;
    }
    self->max_eps = MIN(UCT_PCIE_MAX_EPS, config->max_eps);
    self->eps_init_cnt = 0;
    self->connections = 0;

    /* --- Open SISCI virtual devices --- */
    SCIOpen(&self->vdev_recv, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_recv): %s", SCIGetErrorString(sci_error));
        goto err_destroy_mutex;
    }

    SCIOpen(&self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_ep): %s", SCIGetErrorString(sci_error));
        goto err_free_vdev_recv;
    }

    SCIOpen(&self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_ctl): %s", SCIGetErrorString(sci_error));
        goto err_free_vdev_ep;
    }

    /* --- Control segment: remote ifaces write ack counters into this --- */
    control_segment_size = sizeof(uct_pcie_ctl_t) * self->max_eps;
    ret = uct_pcie_helper_create_seg_set_avail(
        sci_md->sci_virtual_device,
        &self->ctl_segment,
        &self->ctl_segment_map,
        control_segment_size,
        &self->ctl_segment_id,
        (void **)&self->ctls);
    if (ret != 0) {
        ucs_error("Failed to set up control segment");
        goto err_free_vdev_ctl;
    }

    /* --- Receive segment: one AM ring per connection slot (zero-filled) --- */
    ret = uct_pcie_helper_create_user_segment(
        self->vdev_recv,
        &self->recv_segment,
        self->max_eps * uct_pcie_iface_ring_size(self),
        &self->recv_segment_id,
        (void **)&self->recv_buf);
    if (ret != 0) {
        ucs_error("Failed to set up receive segment");
        goto err_remove_ctl_seg;
    }

    /* Initialize connection descriptors; uct_pcie_conn_handler assigns each
     * accepted connection its window of the receive segment. */
    for (i = 0; i < self->max_eps; i++) {
        self->sci_cds[i].cd_status        = UCT_PCIE_CD_AVAILABLE;
        self->sci_cds[i].ep_conn_last_ack = 0;
        self->sci_cds[i].ep_conn_sent_ack = 0;
        self->ctls[i].ep_conn_ack         = 0;
    }

    /* --- DMA engine: a pool of queues, one zero-copy transfer each --- */
    SCIOpen(&self->vdev_dma, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen (vdev_dma): %s", SCIGetErrorString(sci_error));
        goto err_remove_recv_seg;
    }

    self->num_dmas      = 0;
    self->num_free_dmas = 0;
    self->bounce_buf    = NULL;
    self->bounce_memh   = UCT_MEM_HANDLE_NULL;
    self->bounce_slot   = ucs_align_up_pow2(config->zcopy_max + UCT_PCIE_DMA_ALIGN,
                                            UCT_PCIE_DMA_ALIGN);
    self->put_pio_max   = config->put_pio_max;
    self->zcopy_max     = config->zcopy_max;
    self->dma_overhead  = config->dma_overhead;
    self->am_lines_payload_max =
        UCT_PCIE_AM_INLINE_MAX +
        (ucs_max(1, ucs_min(config->am_lines, UCT_PCIE_AM_LINES_MAX)) - 1) *
        UCT_PCIE_AM_LINE_DATA;
    ucs_queue_head_init(&self->flush_reqs);
    for (i = 0; i < ucs_min(config->dma_queues, UCT_PCIE_MAX_DMA_QUEUES); i++) {
        SCICreateDMAQueue(self->vdev_dma, &self->dmas[i].queue,
                          UCT_PCIE_LOCAL_ADAPTER_NO, 1, UCT_PCIE_NO_FLAGS,
                          &sci_error);
        if (sci_error != SCI_ERR_OK) {
            ucs_error("SCICreateDMAQueue: %s", SCIGetErrorString(sci_error));
            goto err_remove_dma_queues;
        }
        self->dmas[i].comp                     = NULL;
        self->dmas[i].ep                       = NULL;
        self->dmas[i].copy_len                 = 0;
        self->free_dmas[self->num_free_dmas++] = i;
        self->num_dmas++;
    }

    /* --- Connection interrupt: entry point for incoming EP connections --- */
    SCICreateDataInterrupt(
        sci_md->sci_virtual_device,
        &self->interrupt,
        UCT_PCIE_LOCAL_ADAPTER_NO,
        &self->interrupt_no,
        uct_pcie_conn_handler,
        self,
        SCI_FLAG_USE_CALLBACK,
        &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCICreateDataInterrupt: %s", SCIGetErrorString(sci_error));
        goto err_remove_dma_queues;
    }

    ucs_debug("iface_addr: %d dev_addr: %d segment_size %zd\n",
        self->interrupt_no,
        self->device_addr,
        self->packet_size_bytes);

    return UCS_OK;

err_remove_dma_queues:
    uct_pcie_iface_remove_dma_queues(self);
    SCIClose(self->vdev_dma, UCT_PCIE_NO_FLAGS, &sci_error);
err_remove_recv_seg:
    uct_pcie_helper_remove_user_segment(
        self->recv_segment, self->recv_buf,
        self->max_eps * uct_pcie_iface_ring_size(self));
err_remove_ctl_seg:
    uct_pcie_helper_remove_seg_set_unavail(self->ctl_segment, self->ctl_segment_map);
err_free_vdev_ctl:
    SCIClose(self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
err_free_vdev_ep:
    SCIClose(self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
err_free_vdev_recv:
    SCIClose(self->vdev_recv, UCT_PCIE_NO_FLAGS, &sci_error);
err_destroy_mutex:
    ucs_free(self->bcopy_buf);
    ucs_free(self->lines_tx_buf);
    ucs_free(self->lines_rx_buf);
    ucs_free(self->lines_tx);
    ucs_free(self->get_buf);
    pthread_mutex_destroy(&self->lock);
err:
    return UCS_ERR_NO_RESOURCE;
}


/**
 * @brief Tear down the interface and free everything allocated during init.
 *
 * self is a uct_pcie_iface_t *   - injected by the UCS_CLASS_CLEANUP_FUNC macro.
 */
static UCS_CLASS_CLEANUP_FUNC(uct_pcie_iface_t)
{
    sci_error_t sci_error;

    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    /* Remove the connection interrupt first: this stops any new incoming
     * connections before we start tearing down segments. */
    SCIRemoveDataInterrupt(self->interrupt, UCT_PCIE_NO_FLAGS, &sci_error);

    /* Tear down every active incoming connection: disconnect from its control
     * segment. */
    for (ssize_t i = 0; i < self->connections; i++) {
        if (self->sci_cds[i].cd_status == UCT_PCIE_CD_READY) {
            self->sci_cds[i].cd_status = UCT_PCIE_CD_RESERVED;
            uct_pcie_disconnect_segment(
                self->sci_cds[i].ctl_segment,
                self->sci_cds[i].ctl_segment_map);
            self->sci_cds[i].cd_status = UCT_PCIE_CD_AVAILABLE;
        }
    }

    /* DMA engine */
    uct_pcie_iface_remove_dma_queues(self);
    SCIClose(self->vdev_dma, UCT_PCIE_NO_FLAGS, &sci_error);
    if (self->bounce_memh != UCT_MEM_HANDLE_NULL) {
        uct_md_mem_dereg(self->super.md, self->bounce_memh);
        munmap(self->bounce_buf, self->num_dmas * self->bounce_slot);
    }

    /* Receive segment (left to the driver while peers are still connected) */
    uct_pcie_helper_remove_user_segment(
        self->recv_segment, self->recv_buf,
        self->max_eps * uct_pcie_iface_ring_size(self));

    /* Control segment */
    uct_pcie_helper_remove_seg_set_unavail(self->ctl_segment, self->ctl_segment_map);

    /* Closing device descriptors */
    SCIClose(self->vdev_ctl, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_ep, UCT_PCIE_NO_FLAGS, &sci_error);
    SCIClose(self->vdev_recv, UCT_PCIE_NO_FLAGS, &sci_error);

    /* All EPs must have been destroyed (and their pending queues purged) before
     * the iface is cleaned up, so the arbiter should be empty here. */
    ucs_arbiter_cleanup(&self->arbiter);

    ucs_free(self->bcopy_buf);
    ucs_free(self->lines_tx_buf);
    ucs_free(self->lines_rx_buf);
    ucs_free(self->lines_tx);
    ucs_free(self->get_buf);
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


/******************************************************************************/
/*** Iface operations *********************************************************/

static ucs_status_t uct_pcie_iface_flush(uct_iface_h tl_iface, unsigned flags,
                                         uct_completion_t *comp)
{
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);

    if (comp != NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    uct_pcie_iface_wc_flush(iface);
    uct_pcie_iface_progress_gets(iface);

    if (iface->num_free_dmas != iface->num_dmas) {
        UCT_TL_IFACE_STAT_FLUSH_WAIT(&iface->super);
        return UCS_INPROGRESS;
    }

    UCT_TL_IFACE_STAT_FLUSH(&iface->super);
    return UCS_OK;
}

/**
 * @brief Enumerate transport devices visible to this MD.
 *
 * Reports a single device entry named "pcie". Node addressing is handled at
 * the EP level via SISCI node IDs, so one device entry covers all remote nodes.
 *
 * @param[in]  md            Memory domain handle.
 * @param[out] devices_p     Allocated array of device descriptors (caller frees).
 * @param[out] num_devices_p Number of entries written.
 */
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

/**
 * @brief UCT v1 reachability bridge.
 *
 * The v1 signature (dev_addr, iface_addr) is required by the iface_ops vtable.
 * uct_base_iface_is_reachable packs those arguments into a params struct and
 * forwards to uct_pcie_iface_is_reachable_v2. All reachability logic lives
 * there; nothing needs to be maintained here.
 *
 * @param[in] tl_iface   Local interface handle.
 * @param[in] dev_addr   Remote device address (forwarded to v2).
 * @param[in] iface_addr Remote interface address (forwarded to v2; unused).
 * @return 1 if reachable, 0 otherwise.
 */
static int uct_pcie_iface_is_reachable(
    const uct_iface_h tl_iface,
    const uct_device_addr_t *dev_addr,
    const uct_iface_addr_t *iface_addr)
{
    return uct_base_iface_is_reachable(tl_iface, dev_addr, iface_addr);
}

/**
 * @brief Write the local SISCI node ID into a device address buffer.
 *
 * The device address is the node ID by which this machine is reachable via
 * SISCI. It is obtained from SCIGetLocalNodeId during iface init.
 *
 * @param[in]  iface  Interface handle.
 * @param[out] addr   Device address buffer (sizeof(uct_pcie_device_addr_t)).
 */
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
 * @brief Fill in the interface address exported to connecting endpoints.
 *
 * The interface address carries the data interrupt number that a remote EP
 * sends its connection request to. Remote memory is addressed through rkeys.
 *
 * @param[in]  tl_iface  Interface handle.
 * @param[out] addr       Interface address buffer (sizeof(uct_pcie_iface_addr_t)).
 */
ucs_status_t uct_pcie_iface_get_address(
    uct_iface_h tl_iface,
    uct_iface_addr_t *addr)
{
    uct_pcie_iface_t *iface    = ucs_derived_of(tl_iface, uct_pcie_iface_t);
    uct_pcie_iface_addr_t *iface_addr = (uct_pcie_iface_addr_t *)addr;

    iface_addr->interrupt_no = iface->interrupt_no;

    return UCS_OK;
}


/**
 * @brief Enable progress callbacks for this interface.
 *
 * Forwards to uct_base_iface_progress_enable, which registers
 * uct_pcie_iface_progress with the worker's progress engine so that
 * incoming AM packets are processed and the pending send arbiter is dispatched.
 *
 * @param[in] iface  Interface handle.
 * @param[in] flags  UCT_PROGRESS_SEND | UCT_PROGRESS_RECV or a subset.
 */
void uct_pcie_iface_progress_enable(uct_iface_h iface, unsigned flags) {
    uct_base_iface_progress_enable(iface, flags);
}

/* Polls for the lines of a line message that arrive after its header */
#define UCT_PCIE_AM_LINES_SPIN 1000

/* Reassemble a UCT_PCIE_AM_FLAG_LINES message, or NULL if not all its lines
 * have arrived yet */
static void *uct_pcie_iface_gather_lines(uct_pcie_iface_t *iface,
                                         uct_pcie_am_hdr_t *packet)
{
    uct_pcie_am_line_t *lines = (uct_pcie_am_line_t*)(packet + 1);
    size_t              rest  = packet->am_length - UCT_PCIE_AM_INLINE_MAX;
    size_t              count = ucs_div_round_up(rest, UCT_PCIE_AM_LINE_DATA);
    uint8_t            *dst   = iface->lines_rx_buf;
    unsigned            spin  = 0;
    size_t              i;

    /* The header line can overtake the others; they are nanoseconds behind,
     * so wait a little here instead of in another progress round */
    for (i = 0; i < count; i++) {
        while (lines[i].seq != packet->seq) {
            if (++spin > UCT_PCIE_AM_LINES_SPIN) {
                return NULL;
            }
            ucs_cpu_relax();
        }
    }
    ucs_memory_cpu_load_fence();

    memcpy(dst, packet->data, UCT_PCIE_AM_INLINE_MAX);
    dst += UCT_PCIE_AM_INLINE_MAX;
    for (i = 0; i < count; i++) {
        memcpy(dst, lines[i].data, ucs_min(rest, UCT_PCIE_AM_LINE_DATA));
        dst  += UCT_PCIE_AM_LINE_DATA;
        rest -= ucs_min(rest, UCT_PCIE_AM_LINE_DATA);
    }
    return iface->lines_rx_buf;
}

static UCS_F_ALWAYS_INLINE void
uct_pcie_iface_send_ack(uct_pcie_iface_t *iface, uct_pcie_conn_desc_t *cd)
{
    uct_pcie_ctl_t line;

    line.ep_conn_ack = cd->ep_conn_last_ack;
    iface->store_line(cd->ctl_buf, &line);
    cd->ep_conn_sent_ack = cd->ep_conn_last_ack;
    iface->wc_dirty      = 1;
}

/**
 * @brief Complete finished DMA transfers and the ep flushes waiting for them.
 *
 * @return Number of transfers completed.
 */
static unsigned uct_pcie_iface_progress_dma(uct_pcie_iface_t *iface)
{
    uct_pcie_flush_req_t  *req;
    ucs_queue_iter_t       iter;
    sci_dma_queue_state_t  state;
    uct_pcie_dma_t        *dma;
    ucs_status_t           status;
    unsigned               count = 0;
    unsigned int           i;

    for (i = 0; i < iface->num_dmas; i++) {
        dma = &iface->dmas[i];
        if (dma->ep == NULL) {
            continue;
        }

        state = SCIDMAQueueState(dma->queue);
        if (state == SCI_DMAQUEUE_DONE) {
            status = UCS_OK;
        } else if ((state == SCI_DMAQUEUE_ERROR) ||
                   (state == SCI_DMAQUEUE_ABORTED)) {
            ucs_error("pcie DMA transfer failed (queue state %d)", state);
            status = UCS_ERR_IO_ERROR;
        } else {
            continue;
        }

        if (dma->copy_len > 0) {
            memcpy(dma->copy_dst, dma->copy_src, dma->copy_len);
            dma->copy_len = 0;
        }
        dma->ep->dma_outstanding--;
        dma->ep                                  = NULL;
        iface->free_dmas[iface->num_free_dmas++] = i;
        if (dma->comp != NULL) {
            uct_invoke_completion(dma->comp, status);
        }
        count++;
    }

    ucs_queue_for_each_safe(req, iter, &iface->flush_reqs, queue) {
        if (req->ep->dma_outstanding == 0) {
            ucs_queue_del_iter(&iface->flush_reqs, iter);
            uct_invoke_completion(req->comp, UCS_OK);
            ucs_free(req);
        }
    }

    return count;
}

/**
 * @brief Arbiter callback: retry a previously blocked AM send.
 *
 * Called during iface_progress for each pending request.
 */
static ucs_arbiter_cb_result_t
uct_pcie_ep_dispatch_pending(
    ucs_arbiter_t       *arbiter,
    ucs_arbiter_group_t *group,
    ucs_arbiter_elem_t  *elem,
    void                *arg)
{
    uct_pending_req_t *req = ucs_container_of(elem, uct_pending_req_t, priv);
    /* req->func is the AM send function (am_short, am_bcopy, am_zcopy, etc.)
     * that originally returned UCS_ERR_NO_RESOURCE. */
    ucs_status_t status = req->func(req);

    /* Report the retry result to the arbiter using its expected return codes. */
    if (status == UCS_OK) {
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    } else {
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
 * Note: Only processes packets in order. Stops at the first unposted
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

    uct_pcie_iface_wc_flush(iface);
    count += uct_pcie_iface_progress_gets(iface);

    if (iface->num_free_dmas != iface->num_dmas) {
        count += uct_pcie_iface_progress_dma(iface);
    }

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
            if (packet->seq != cd->ep_conn_last_ack + 1) {
                break;
            }
            ucs_memory_cpu_load_fence();

            if (packet->am_flags & UCT_PCIE_AM_FLAG_INLINE) {
                packet_payload_ptr = (void*)packet->data;
            } else if (packet->am_flags & UCT_PCIE_AM_FLAG_LINES) {
                packet_payload_ptr = uct_pcie_iface_gather_lines(iface, packet);
                if (packet_payload_ptr == NULL) {
                    break; /* some lines still in flight, retry next time */
                }
            } else {
                packet_payload_ptr = (void*)(packet + 1);
            }

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

            /* The slot is free again; tell the sender in batches */
            cd->ep_conn_last_ack++;
            if (cd->ep_conn_last_ack - cd->ep_conn_sent_ack >= iface->ack_batch) {
                uct_pcie_iface_send_ack(iface, cd);
            }

            count++;
        }

        if (cd->ep_conn_last_ack != cd->ep_conn_sent_ack) {
            uct_pcie_iface_send_ack(iface, cd);
        }
    }

    /* Retry blocked sends. ack counters advanced above, which may have opened
     * flow-control slots. Call chain: ucs_arbiter_dispatch ->
     * uct_pcie_ep_dispatch_pending -> req->func (am_short / am_bcopy / am_zcopy).
     * The send function itself checks whether there is now room in the ring
     * buffer. Quota=1 means at most one element per EP group per tick,
     * preventing any single EP from starving others. */
    ucs_arbiter_dispatch(&iface->arbiter, 1, uct_pcie_ep_dispatch_pending, NULL);

    return count;
}

/**
 * @brief Report interface capabilities and performance parameters.
 *
 * Advertises AM and put operations in short, bcopy, and zcopy variants, and
 * get as DMA zero-copy. Atomics are not advertised (see [NO_ATOMICS] in
 * pcie_ep.c). The latency and bandwidth figures describe CPU stores; DMA
 * figures are reported per operation by uct_pcie_iface_estimate_perf.
 *
 * @param[in]  tl_iface  Interface handle.
 * @param[out] attr       Attribute structure to populate.
 */
static ucs_status_t uct_pcie_iface_query(
    uct_iface_h tl_iface,
    uct_iface_attr_t *attr)
{
    size_t am_max;
    uct_pcie_iface_t *iface = ucs_derived_of(tl_iface, uct_pcie_iface_t);

    uct_base_iface_query(ucs_derived_of(tl_iface, uct_base_iface_t), attr);

    attr->cap.flags = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                      UCT_IFACE_FLAG_INTER_NODE       |
                      UCT_IFACE_FLAG_CB_SYNC          |
                      UCT_IFACE_FLAG_AM_SHORT         |
                      UCT_IFACE_FLAG_AM_BCOPY         |
                      UCT_IFACE_FLAG_AM_ZCOPY         |
                      UCT_IFACE_FLAG_PUT_SHORT        |
                      UCT_IFACE_FLAG_PUT_BCOPY        |
                      UCT_IFACE_FLAG_PUT_ZCOPY        |
                      UCT_IFACE_FLAG_GET_ZCOPY        |
                      UCT_IFACE_FLAG_PENDING;
    if (iface->get_pio_max > 0) {
        attr->cap.flags        |= UCT_IFACE_FLAG_GET_BCOPY;
        attr->cap.get.max_bcopy = iface->get_pio_max;
    }
    attr->cap.event_flags = 0;

    attr->device_addr_len = sizeof(uct_pcie_device_addr_t);
    attr->iface_addr_len  = sizeof(uct_pcie_iface_addr_t);
    attr->ep_addr_len     = 0;

    /* AM limits: each slot holds the header line followed by user data.
     * All variants share the same slot-remainder bound. */
    am_max = iface->packet_size_bytes - sizeof(uct_pcie_am_hdr_t);
    attr->cap.am.max_short = am_max;
    attr->cap.am.max_bcopy = am_max;
    attr->cap.am.min_zcopy = 0;
    attr->cap.am.max_zcopy = am_max;
    attr->cap.am.max_iov   = 10;
    attr->cap.am.max_hdr   = am_max;

    /* Put: short and bcopy are CPU stores through the mapping of the remote
     * registered range, zcopy is DMA from the registered local buffer (or CPU
     * stores up to put_pio_max). Get is DMA, or streaming CPU loads up to
     * get_pio_max (bcopy). */
    attr->cap.put.max_short        = UCT_PCIE_MAX_PUT_SHORT;
    attr->cap.put.max_bcopy        = iface->zcopy_max;
    attr->cap.put.min_zcopy        = 0;
    attr->cap.put.max_zcopy        = iface->zcopy_max;
    attr->cap.put.opt_zcopy_align  = 1;
    attr->cap.put.align_mtu        = 1;
    attr->cap.put.max_iov          = 1;

    attr->cap.get.min_zcopy        = 0;
    attr->cap.get.max_zcopy        = iface->zcopy_max;
    attr->cap.get.opt_zcopy_align  = 1;
    attr->cap.get.align_mtu        = 1;
    attr->cap.get.max_iov          = 1;

    /* CPU stores, measured fpga1614 <-> fpga1610 (MXH930, Gen4 x16): 8-byte
     * AM one way 1.04 us, a stream of them 0.05-0.07 us each, 20.4 GB/s from
     * one core. With the former 1 us overhead UCP preferred rendezvous (DMA)
     * from 32 KiB on, at twice the time per message. */
    attr->latency             = ucs_linear_func_make(1.0e-6, 0);
    attr->bandwidth.dedicated = 20.4 * UCS_GBYTE;
    attr->bandwidth.shared    = 0;
    attr->overhead            = 0.07e-6;
    attr->priority            = 0;

    return UCS_OK;
}

static uct_iface_ops_t uct_pcie_iface_ops = {
    .ep_put_short             = uct_pcie_ep_put_short,
    .ep_put_bcopy             = uct_pcie_ep_put_bcopy,
    .ep_put_zcopy             = uct_pcie_ep_put_zcopy,
    .ep_get_bcopy             = uct_pcie_ep_get_bcopy,
    .ep_get_zcopy             = uct_pcie_ep_get_zcopy,
    
    .ep_am_short              = uct_pcie_ep_am_short,
    .ep_am_short_iov          = uct_pcie_ep_am_short_iov,
    .ep_am_bcopy              = uct_pcie_ep_am_bcopy,
    .ep_am_zcopy              = uct_pcie_ep_am_zcopy,
    
    /* [NO_ATOMICS] Transport will not implement atomics. */
    .ep_atomic_cswap64        = uct_pcie_ep_atomic_cswap64,
    .ep_atomic64_post         = uct_pcie_ep_atomic64_post,
    .ep_atomic64_fetch        = uct_pcie_ep_atomic64_fetch,
    .ep_atomic_cswap32        = uct_pcie_ep_atomic_cswap32,
    .ep_atomic32_post         = uct_pcie_ep_atomic32_post,
    .ep_atomic32_fetch        = uct_pcie_ep_atomic32_fetch,

    .ep_flush                 = uct_pcie_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)ucs_empty_function_return_success,
    .ep_pending_add           = uct_pcie_ep_pending_add,
    .ep_pending_purge         = uct_pcie_ep_pending_purge,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_pcie_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_pcie_ep_t),
    .iface_flush              = uct_pcie_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = uct_pcie_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_pcie_iface_progress,
    .iface_event_arm          = (uct_iface_event_arm_func_t)ucs_empty_function_return_success,
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
    SCIInitialize(UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIInitialize error: %s", SCIGetErrorString(sci_error));
    }
}

UCS_STATIC_CLEANUP
{
    SCITerminate();
}
