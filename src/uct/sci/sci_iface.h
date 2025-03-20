#ifndef UCT_SCI_IFACE_H
#define UCT_SCI_IFACE_H

#include "pthread.h"

#include <uct/base/uct_iface.h>

#include <sisci_error.h> //TODO
#include <sisci_api.h>

#define UCT_SCI_NAME "sci"

#define UCT_SCI_LOCAL_ADAPTER_NO 0
#define UCT_SCI_NO_FLAGS 0
#define UCT_SCI_NO_CALLBACK 0
#define UCT_SCI_MAX_EPS 28

#define DEBUG 0

#if defined(DEBUG) && DEBUG > 0
 #define DEBUG_PRINT(fmt, args...) fprintf(stdout, "%d: %s:%d:%s(): " fmt, \
    getpid(), __FILE__, __LINE__, __func__, ##args)
#else
 #define DEBUG_PRINT(fmt, args...) /* Don't do anything in release builds */
#endif

typedef struct {
    unsigned int interrupt_no; /* Listening port of iface */
} UCS_S_PACKED uct_sci_iface_addr_t;

typedef struct {
    unsigned int node_id;
} UCS_S_PACKED uct_sci_device_addr_t;

typedef struct {
    uint32_t     ep_conn_ack;
} uct_sci_ctl_t;

typedef struct {
    uint8_t     am_id;
    uint8_t     am_message_posted;
    unsigned    am_length;
} UCS_S_PACKED uct_sci_am_hdr_t;

typedef enum {
    UCT_SCI_CD_AVAILABLE,
    UCT_SCI_CD_RESERVED,
    UCT_SCI_CD_READY,
} conn_desc_status_t;

/*
 * Connection Descriptor,
 *  
 * Each incoming connection gets assigned a different section of the segment.
 * We are using one large segment, with a single map for this segment. So each cd is given an offset
 * into the global offset.
 */
typedef struct {
    conn_desc_status_t      cd_status;
    int                     size;   /* size */
    int                     remote_node;
    uint32_t                ep_conn_last_ack;
    
    /*        rx info          */
    uint32_t                ep_conn_offset; /* start of our map in the global segment */
    void*                   cd_buf;
    uct_sci_am_hdr_t*       packet;
    
    /*    Control info        */
    uint32_t                ctl_segment_id;
    sci_remote_segment_t    ctl_segment;
    sci_map_t               ctl_segment_map;
    uct_sci_ctl_t*          ctl_buf;
} uct_sci_conn_desc_t;

typedef struct {
    int     node_id;
    int     interrupt;
    int     ctl_segment_id;
    int     ep_conn_index;
} uct_sci_conn_req_t;

typedef struct {
    unsigned int node_id;
    unsigned int segment_id;
    unsigned int ep_conn_offset;
    unsigned int packet_size_bytes;
    unsigned int packet_queue_len;
} uct_sci_conn_ans_t;


void sci_testing();

typedef struct {
    uct_iface_config_t    super;
    size_t                packet_size_bytes;      /* Maximal send size */
    unsigned int          max_eps;
    unsigned int          packet_queue_len;

} uct_sci_iface_config_t;

typedef struct {
    uct_sci_am_hdr_t              super;     /* UCT TCP AM header */
    uct_completion_t              *comp;     /* Local UCT completion object */
    size_t                        iov_index; /* Current IOV index */
    size_t                        iov_cnt;   /* Number of IOVs that should be sent */
    struct iovec                  iov[0];    /* IOVs that should be sent */
} uct_sci_ep_zcopy_tx_t;

typedef struct {
    uct_base_iface_t            super;
    unsigned int                segment_id;           /* Unique identifier for the instance */
    unsigned int                device_addr; //nodeID
    size_t                      packet_size_bytes;    /* Maximum size for payload */
    unsigned int                max_eps;
    //ucs_mpool_t                 msg_mp;       /* Messages memory pool */
    void*                       recv_buffer;
    sci_local_segment_t         local_segment; 
    sci_map_t                   local_map;
    sci_dma_queue_t             dma_queue;
    sci_local_segment_t         dma_segment;
    sci_map_t                   dma_map;
    void*                       dma_buffer; /* Move up to DMA etc */
    uct_sci_conn_desc_t         sci_cds[UCT_SCI_MAX_EPS];
    sci_local_data_interrupt_t  interrupt; 
    unsigned int                interrupt_no;
    uint32_t                    packet_queue_len;

    /*      ctl segment, used for control during runtime between processes  */
    sci_desc_t                  vdev_ep; //Vdev used for outgoing eps
    sci_desc_t                  vdev_ctl; //vdev used for control
    pthread_mutex_t             lock;
    unsigned int                eps_init_cnt;
    unsigned int                ctl_segment_id;
    unsigned int                connections;
    sci_local_segment_t         ctl_segment;
    sci_map_t                   ctl_segment_map;
    uct_sci_ctl_t*              ctls;
} uct_sci_iface_t;



#endif