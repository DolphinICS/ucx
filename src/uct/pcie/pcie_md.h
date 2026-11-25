/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_PCIE_MD_H
#define UCT_PCIE_MD_H

#include <uct/api/uct_def.h>
#include <uct/base/uct_md.h>
#include <ucs/datastruct/list.h>

#include <sisci_api.h>
#include <pthread.h>

/* One RMA segment per MD that mem_alloc carves up. mkey_pack puts its node,
 * ID and base VA in the rkey. */
typedef struct {
    uct_md_t            super;
    sci_desc_t          sci_virtual_device;
    unsigned int        node_id;
    /* RMA segment, pre-allocated at md_open with UCT_PCIE_RMA_SEG_SIZE bytes.
     * mem_alloc hands out ranges of it, peers map it through the rkey */
    sci_local_segment_t rma_seg;
    sci_map_t           rma_seg_map;
    void                *rma_buf; /* local VA of segment start (this process) */
    unsigned int        rma_seg_id;
    /* Unallocated ranges of the segment, uct_pcie_free_range_t sorted by
     * offset, with no two adjacent */
    ucs_list_link_t     free_ranges;
    pthread_mutex_t     alloc_lock; /* Protects free_ranges */
} uct_pcie_md_t;

/* A range of the RMA segment that mem_alloc can hand out */
typedef struct {
    ucs_list_link_t list;
    size_t          offset;
    size_t          length;
} uct_pcie_free_range_t;

/* Packed rkey: where the peer's RMA segment is */
typedef struct {
    uint32_t node_id;
    uint32_t seg_id;
    /* VA of the segment on its node. remote_addr - base is the offset into
     * the segment. */
    uint64_t base;
} UCS_S_PACKED uct_pcie_packed_rkey_t;

/* SISCI descriptor that rkey_unpack connects through */
typedef struct {
    ucs_list_link_t list;
    sci_desc_t      sd;
    unsigned int    segments; /* Connected through sd */
} uct_pcie_rkey_desc_t;

/* Unpacked rkey, also the uct_rkey_t: a peer's RMA segment mapped in this
 * process. Every rkey for the same segment shares one, see
 * uct_pcie_md_rkey_unpack. */
typedef struct {
    ucs_list_link_t      list;
    unsigned int         refcount;
    uct_pcie_rkey_desc_t *desc;
    unsigned int         node_id;
    unsigned int         seg_id;
    uint64_t             base;
    sci_remote_segment_t segment;
    sci_map_t            map;
    /* Local mapping of the segment; every access crosses PCIe */
    void                 *va;
} uct_pcie_rkey_t;

/* Handle returned by mem_alloc, a range of the MD's RMA segment. mem_free
 * gives the range back to the MD. */
typedef struct {
    void   *ptr; /* md->rma_buf + offset */
    size_t length;
} uct_pcie_mem_handle_t;


extern uct_component_t uct_pcie_component;

#endif
