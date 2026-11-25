/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_PCIE_IFACE_H
#define UCT_PCIE_IFACE_H

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/list.h>

#include <sisci_error.h>
#include <sisci_api.h>
#include <pthread.h>

#define UCT_PCIE_NAME "pcie"

#define UCT_PCIE_LOCAL_ADAPTER_NO 0
#define UCT_PCIE_NO_FLAGS         0
/* Size of sci_cds[]. The MAX_NUM_EPS and MAX_EPS config defaults must match */
#define UCT_PCIE_MAX_EPS 32

/* Maximum bytes for put_short. put_short and put_bcopy both copy into the
 * PIO window, so this limit only decides where the caller switches from one
 * to the other. */
#define UCT_PCIE_MAX_PUT_SHORT 4096

/* Size of the MD's RMA segment and of the iface's DMA staging segment. Needs
 * the driver's ntb_memory_preallocation_size_mb raised above its default. */
#define UCT_PCIE_RMA_SEG_SIZE (16 * 1024 * 1024)

/* get above this size uses DMA instead of PIO reads */
#define UCT_PCIE_GET_DMA_THRESHOLD 255

/* put_zcopy from the MD pool above this size uses DMA. PIO writes are posted,
 * so DMA only wins for large transfers. */
#define UCT_PCIE_PUT_DMA_THRESHOLD (512 * 1024)

/* DMA queue length. Transfers are waited for before the op returns. */
#define UCT_PCIE_DMA_QUEUE_MAX_ENTRIES 10

/* Maximum iov count for active messages */
#define UCT_PCIE_AM_MAX_IOV 10

/* How long connection setup retries a SISCI connect, or waits for the peer's
 * answer, before giving up */
#define UCT_PCIE_CONNECT_TIMEOUT_MS 10000

/* Iface address exchanged before EP creation. The MD has a single RMA segment
 * per process, so its identity is carried here rather than in the rkey, and
 * rkey_packed_size is 0. */
typedef struct {
    unsigned int interrupt_no; /* Connection interrupt */

    /* SISCI segment ID of the MD's RMA segment */
    unsigned int rma_seg_id;

    /* Local VA of the RMA segment on this node. mem_alloc returns addresses
     * inside the segment, so a peer turns remote_addr into a segment offset
     * with remote_addr - rma_base. */
    uint64_t     rma_base;
} UCS_S_PACKED uct_pcie_iface_addr_t;

typedef struct {
    unsigned int node_id;
} UCS_S_PACKED uct_pcie_device_addr_t;

/* One EP's slot in its iface's control segment, written by the peer */
typedef struct {
    volatile uint64_t ep_conn_ack;
    /* Answer to the connection request: the receive segment the peer created
     * for this EP. recv_seg_id is written first, then conn_answered. */
    volatile uint32_t recv_seg_id;
    volatile uint32_t conn_answered;
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
} uct_pcie_conn_desc_status_t;

/* Incoming connection, owns the receive segment created for it */
typedef struct {
    uct_pcie_conn_desc_status_t cd_status;
    uint64_t                    ep_conn_last_ack;

    sci_local_segment_t         recv_segment;
    sci_map_t                   recv_segment_map;
    unsigned int                recv_segment_id;

    /* Mapped base of recv_segment, the ring buffer the remote EP writes to */
    uint8_t                     *packet_queue_buf;

    /* Peer EP's ack slot in its iface's control segment */
    uint32_t                    ctl_segment_id;
    sci_remote_segment_t        ctl_segment;
    sci_map_t                   ctl_segment_map;
    uct_pcie_ctl_t              *ctl_buf;
} uct_pcie_conn_desc_t;

typedef struct {
    int node_id;
    int ctl_segment_id;
    int ep_conn_index;
} uct_pcie_conn_req_t;

typedef struct {
    uct_iface_config_t super;
    /* AM slot size in bytes, the largest AM that fits */
    size_t             packet_size_bytes;
    /* Number of AM slots per connection */
    unsigned int       packet_queue_len;
    /* Maximum number of endpoints supported by the iface */
    unsigned int       max_eps;
} uct_pcie_iface_config_t;

typedef struct {
    uct_base_iface_t           super;

    /* Local SISCI node ID, obtained from SCIGetLocalNodeId at init time. */
    unsigned int               device_addr;

    /* Per-slot byte size for the AM ring buffer. Each AM send occupies one
     * slot; the slot holds a uct_pcie_am_hdr_t followed by the payload. */
    size_t                     packet_size_bytes;
    /* Number of ring-buffer slots per endpoint connection. */
    uint32_t                   packet_queue_len;
    /* Maximum number of simultaneously connected endpoints. */
    unsigned int               max_eps;

    /* DMA for get and put_zcopy. Transfers finish before the op returns, so
     * at most one is queued. */
    sci_dma_queue_t            dma_queue;
    sci_local_segment_t        dma_segment;
    sci_map_t                  dma_map;
    void                       *dma_buffer;

    /* One descriptor per incoming connection */
    uct_pcie_conn_desc_t       sci_cds[UCT_PCIE_MAX_EPS];

    /* Data interrupt that receives incoming connection requests. Its number
     * is published in iface_addr so remote EPs know where to send them. */
    sci_local_data_interrupt_t interrupt;
    unsigned int               interrupt_no;

    /* A SISCI descriptor holds at most 32 resources of a type, so each
     * segment role gets its own to allow 32 EPs */
    sci_desc_t                 vdev_recv; /* per-connection recv segments */
    sci_desc_t                 vdev_ep; /* AM receive segment connections */
    sci_desc_t                 vdev_rma; /* RMA segment connections */
    sci_desc_t                 vdev_ctl; /* control segment connections */

    /* Protects connections count and sci_cds slot allocation in the
     * connection handler, which runs on the SISCI callback thread. */
    pthread_mutex_t            lock;

    /* Pending sends, retried from progress */
    ucs_arbiter_t              arbiter;

    /* EPs waiting for the peer's answer, polled from progress */
    ucs_list_link_t            connecting_eps;

    /* Monotonically increasing count of EPs ever created on this iface.
     * Used to assign each new EP a unique ep_conn_index. */
    unsigned int               eps_init_cnt;
    /* Number of currently active incoming connections. */
    volatile unsigned int      connections;

    /* Control segment: a local segment that remote EPs map and write
     * acknowledgement counters into. One uct_pcie_ctl_t slot per EP. */
    unsigned int               ctl_segment_id;
    sci_local_segment_t        ctl_segment;
    sci_map_t                  ctl_segment_map;
    uct_pcie_ctl_t             *ctls;
} uct_pcie_iface_t;

#endif
