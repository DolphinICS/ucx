#ifndef UCT_PCIE_IFACE_H
#define UCT_PCIE_IFACE_H

#include "pthread.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>

#include <sisci_error.h> //TODO
#include <sisci_api.h>

#define UCT_PCIE_NAME "pcie"

#define UCT_PCIE_LOCAL_ADAPTER_NO 0
#define UCT_PCIE_NO_FLAGS 0
#define UCT_PCIE_NO_CALLBACK 0
#define UCT_PCIE_MAX_EPS 28

/* Maximum bytes for put_short. This threshold is arbitrary for a PIO
 * transport: put_short and put_bcopy both do a memcpy into the MMIO window,
 * so there is no physical reason to prefer one over the other at any
 * particular size. The UCT distinction is API shape: put_short takes a plain
 * (buffer, length) pair while put_bcopy takes a pack callback that writes
 * directly into the destination. 4096 is a reasonable default; it can be
 * raised further without consequence. */
#define UCT_PCIE_MAX_PUT_SHORT 4096

/* Size of the shared segment pre-allocated in the MD.  All mem_alloc
 * calls draw from this pool.  1 MB is generous for typical put workloads. */
#define UCT_PCIE_RMA_SEG_SIZE (1024 * 1024)

/* Iface address exchanged OOB before EP creation.
 * Were ucx_perftest to follow the UCT rkey contract, this struct would only
 * carry interrupt_no. The two extra fields are workarounds for libperf bugs. */
typedef struct {
    unsigned int interrupt_no;   /* Connection interrupt */

    /* [LIBPERF_RKEY_QUIRK] quirk 1: rma_seg_id belongs in the rkey, not here.
     * libperf sends the rkey buffer to the remote side before calling mkey_pack,
     * so the remote side always receives zeros. Segment identity therefore cannot
     * travel via rkey and is piggybacked on iface_addr instead. */
    unsigned int rma_seg_id;

    /* [LIBPERF_RKEY_QUIRK] quirk 2: this field exists only to cancel noise
     * introduced by a second libperf quirk. libperf dereferences *address_p
     * locally (memset, verify), so mem_alloc must return a real local VA
     * (rma_buf_local + offset) rather than a plain segment offset. That bakes
     * rma_buf_local into the value the initiator receives as remote_addr.
     * rma_local_base = rma_buf_local on the target, sent here so the initiator
     * can recover the plain offset:  seg_offset = remote_addr - rma_local_base
     * Were libperf not to dereference *address_p, remote_addr could be a plain
     * offset from zero and this field would not exist. */
    uint64_t     rma_local_base;
} UCS_S_PACKED uct_pcie_iface_addr_t;

typedef struct {
    unsigned int node_id;
} UCS_S_PACKED uct_pcie_device_addr_t;

typedef struct {
    volatile uint64_t ep_conn_ack;
} uct_pcie_ctl_t;

typedef struct {
    uint8_t          am_id;
    volatile uint8_t am_message_posted;
    unsigned         am_length;
} UCS_S_PACKED uct_pcie_am_hdr_t;

typedef enum {
    UCT_PCIE_CD_AVAILABLE,
    UCT_PCIE_CD_RESERVED,
    UCT_PCIE_CD_READY,
} conn_desc_status_t;

/*
 * Connection Descriptor,
 *  
 * Each incoming connection gets assigned a different section of the segment.
 * We are using one large segment, with a single map for this segment. So
 * each cd is given an offset into the global offset.
 */
typedef struct {
    conn_desc_status_t      cd_status;
    int                     remote_node;
    uint64_t                ep_conn_last_ack;
    
    /* Data transfer (used to send the actual data) */

    /* endpoint's offset into iface's segment
     * ep_conn_offset = (iface's sci_cd index) * (packet_queue size in bytes)
     * 
     * for context, packet queue size in bytes
     * is (self->packet_size_bytes * self->packet_queue_len)
     */
    uint32_t                ep_conn_offset;
    
    /* Buffer with circular buffer made to hold a queue of packets
     * packet_queue_buf = iface receive buffer + ep_conn_offset
     */
    uint8_t*                packet_queue_buf;
    
    /* Control segment (used by iface to send signals to endpoints).
     * Currently, the only signals are ack messages */
    uint32_t                ctl_segment_id;
    sci_remote_segment_t    ctl_segment;
    sci_map_t               ctl_segment_map;
    uct_pcie_ctl_t*          ctl_buf;
} uct_pcie_conn_desc_t;

typedef struct {
    int     node_id;
    int     interrupt;
    int     ctl_segment_id;
    int     ep_conn_index;
} uct_pcie_conn_req_t;

typedef struct {
    unsigned int segment_id;
    unsigned int ep_conn_offset;
} uct_pcie_conn_ans_t;

typedef struct {
    uct_iface_config_t    super;
    /* Size of packet in bytes (Maximal send size) */
    size_t                packet_size_bytes;
    /* Number of packets in the packet queue */
    unsigned int          packet_queue_len;
    /* Maximum number of endpoints supported by the iface */
    unsigned int          max_eps;

} uct_pcie_iface_config_t;

typedef struct {
    /* UCT TCP AM header */
    uct_pcie_am_hdr_t              super;
    /* Local UCT completion object */
    uct_completion_t              *comp;
    /* Current IOV index */
    size_t                        iov_index;
    /* Number of IOVs that should be sent */
    size_t                        iov_cnt;
    /* IOVs that should be sent */
    struct iovec                  iov[0];
} uct_pcie_ep_zcopy_tx_t;

typedef struct {
    uct_base_iface_t            super;
    /* Unique identifier for the instance */
    unsigned int                segment_id;
    /* Node ID */
    unsigned int                device_addr;
    /* Maximum size for payload */
    size_t                      packet_size_bytes;
    uint32_t                    packet_queue_len;
    unsigned int                max_eps;

    /* Messages memory pool */
    //ucs_mpool_t                 msg_mp;
    
    uint8_t*                    recv_buffer;
    
    sci_local_segment_t         local_segment; 
    sci_map_t                   local_map;
    
    sci_dma_queue_t             dma_queue;
    sci_local_segment_t         dma_segment;
    sci_map_t                   dma_map;
    void*                       dma_buffer;

    uct_pcie_conn_desc_t         sci_cds[UCT_PCIE_MAX_EPS];

    sci_local_data_interrupt_t  interrupt; 
    unsigned int                interrupt_no;

    sci_desc_t                  vdev_ep;
    sci_desc_t                  vdev_ctl;
    
    pthread_mutex_t             lock;

    /* Arbiter for pending send requests. When an AM send returns
     * UCS_ERR_NO_RESOURCE (flow-control window full), ep_pending_add queues
     * the request here. uct_pcie_iface_progress drains it each tick. */
    ucs_arbiter_t               arbiter;

    unsigned int                eps_init_cnt;
    volatile unsigned int       connections;
    
    /* ctl segment. Used to send signals from iface to endpoint. Currently the
     * only such signal to go in that direction is the ack counter*/
    unsigned int                ctl_segment_id;
    sci_local_segment_t         ctl_segment;
    sci_map_t                   ctl_segment_map;
    uct_pcie_ctl_t*              ctls;
} uct_pcie_iface_t;



#endif