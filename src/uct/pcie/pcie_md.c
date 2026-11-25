/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcie_md.h"
#include "pcie_iface.h"
#include "pcie_sisci_helper.h"

#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>
#include <ucs/sys/ptr_arith.h>
#include <ucs/arch/cpu.h>

#include <pthread.h>


/* A SISCI descriptor holds at most 32 connected remote segments: the 33rd
 * SCIConnectSegment on one fails with "Out of local resources" */
#define UCT_PCIE_RKEY_DESC_MAX_SEGMENTS 32

/* Peers' RMA segments mapped by rkey_unpack, and the descriptors they are
 * connected through. Process-wide, since rkey_unpack has no MD or iface. */
static pthread_mutex_t uct_pcie_rkey_lock = PTHREAD_MUTEX_INITIALIZER;
static UCS_LIST_HEAD(uct_pcie_rkeys);
static UCS_LIST_HEAD(uct_pcie_rkey_descs);

static void uct_pcie_md_close(uct_md_h md)
{
    uct_pcie_md_t *sci_md = ucs_derived_of(md, uct_pcie_md_t);
    sci_error_t sci_error;

    uct_pcie_segment_remove_available(sci_md->rma_seg, sci_md->rma_seg_map);

    SCIClose(sci_md->sci_virtual_device, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIClose failed: %s", SCIGetErrorString(sci_error));
    }

    ucs_free(sci_md);
}

static ucs_status_t uct_pcie_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    attr->flags            = UCT_MD_FLAG_ALLOC | UCT_MD_FLAG_NEED_RKEY;
    attr->max_alloc        = 0;
    attr->reg_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->alloc_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->detect_mem_types = 0;
    attr->max_reg          = ULONG_MAX;
    attr->rkey_packed_size = sizeof(uct_pcie_packed_rkey_t);
    attr->reg_cost         = ucs_linear_func_make(0, 0);
    memset(&attr->local_cpus, 0xff, sizeof(attr->local_cpus));

    return UCS_OK;
}

/* Bump-allocate from the MD's RMA segment. Space is not reused until the MD
 * is closed. */
static ucs_status_t
uct_pcie_mem_alloc(uct_md_h uct_md, size_t *length_p, void **address_p,
                   ucs_memory_type_t mem_type, ucs_sys_device_t sys_dev,
                   unsigned flags, const char *alloc_name, uct_mem_h *memh_p)
{
    uct_pcie_md_t *md = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_mem_handle_t *handle;
    size_t offset;
    size_t length;

    /* Keep allocations on separate cache lines */
    length = ucs_align_up_pow2(*length_p, UCS_SYS_CACHE_LINE_SIZE);

    handle = ucs_malloc(sizeof(*handle), "uct_pcie_mem_alloc");
    if (handle == NULL) {
        ucs_error("failed to allocate uct_pcie_mem_handle_t");
        return UCS_ERR_NO_MEMORY;
    }

    offset = md->rma_allocated;
    if (offset + length > UCT_PCIE_RMA_SEG_SIZE) {
        ucs_error("pcie rma segment exhausted (%zu of %d bytes needed)",
                  offset + length, UCT_PCIE_RMA_SEG_SIZE);
        ucs_free(handle);
        return UCS_ERR_NO_MEMORY;
    }

    md->rma_allocated += length;
    handle->ptr        = UCS_PTR_BYTE_OFFSET(md->rma_buf, offset);
    handle->length     = length;

    *memh_p    = handle;
    *address_p = handle->ptr;

    return UCS_OK;
}

static ucs_status_t uct_pcie_mem_free(uct_md_h md, uct_mem_h memh)
{
    ucs_free(memh);
    return UCS_OK;
}

/* All memory from mem_alloc is in the MD's one RMA segment, so the rkey is the
 * same for every memh */
static ucs_status_t
uct_pcie_md_mkey_pack(uct_md_h uct_md, uct_mem_h memh, void *address,
                      size_t length, const uct_md_mkey_pack_params_t *params,
                      void *rkey_buffer)
{
    uct_pcie_md_t *md                   = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_packed_rkey_t *packed_rkey = rkey_buffer;

    packed_rkey->node_id = md->node_id;
    packed_rkey->seg_id  = md->rma_seg_id;
    packed_rkey->base    = (uintptr_t)md->rma_buf;
    return UCS_OK;
}

/* Reserve room for one segment on a descriptor, opening a new descriptor if
 * all are full. Called with uct_pcie_rkey_lock held. */
static uct_pcie_rkey_desc_t *uct_pcie_rkey_desc_get(void)
{
    uct_pcie_rkey_desc_t *desc;
    sci_error_t sci_error;

    ucs_list_for_each(desc, &uct_pcie_rkey_descs, list) {
        if (desc->segments < UCT_PCIE_RKEY_DESC_MAX_SEGMENTS) {
            desc->segments++;
            return desc;
        }
    }

    desc = ucs_malloc(sizeof(*desc), "uct_pcie_rkey_desc_t");
    if (desc == NULL) {
        ucs_error("failed to allocate uct_pcie_rkey_desc_t");
        return NULL;
    }

    SCIOpen(&desc->sd, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen failed: %s", SCIGetErrorString(sci_error));
        ucs_free(desc);
        return NULL;
    }

    desc->segments = 1;
    ucs_list_add_tail(&uct_pcie_rkey_descs, &desc->list);
    ucs_debug("opened rkey descriptor %p", desc);
    return desc;
}

/* Give back room reserved by uct_pcie_rkey_desc_get, closing the descriptor
 * once nothing uses it. Called with uct_pcie_rkey_lock held. */
static void uct_pcie_rkey_desc_put(uct_pcie_rkey_desc_t *desc)
{
    sci_error_t sci_error;

    if (--desc->segments > 0) {
        return;
    }

    ucs_debug("closing rkey descriptor %p", desc);
    ucs_list_del(&desc->list);
    SCIClose(desc->sd, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIClose failed: %s", SCIGetErrorString(sci_error));
    }
    ucs_free(desc);
}

/* Map the peer's RMA segment, or take another reference on it if this process
 * has it mapped already. One connect attempt: the peer just packed the rkey,
 * so its segment should be there. */
static ucs_status_t
uct_pcie_md_rkey_unpack(uct_component_t *component, const void *rkey_buffer,
                        const uct_rkey_unpack_params_t *params,
                        uct_rkey_t *rkey_p, void **handle_p)
{
    const uct_pcie_packed_rkey_t *packed_rkey = rkey_buffer;
    uct_pcie_rkey_t *rkey;
    ucs_status_t status;
    sci_error_t sci_error;

    pthread_mutex_lock(&uct_pcie_rkey_lock);

    ucs_list_for_each(rkey, &uct_pcie_rkeys, list) {
        if ((rkey->node_id == packed_rkey->node_id) &&
            (rkey->seg_id == packed_rkey->seg_id) &&
            (rkey->base == packed_rkey->base)) {
            rkey->refcount++;
            status = UCS_OK;
            goto out;
        }
    }

    rkey = ucs_malloc(sizeof(*rkey), "uct_pcie_rkey_t");
    if (rkey == NULL) {
        ucs_error("failed to allocate uct_pcie_rkey_t");
        status = UCS_ERR_NO_MEMORY;
        goto out_unlock;
    }

    rkey->desc = uct_pcie_rkey_desc_get();
    if (rkey->desc == NULL) {
        status = UCS_ERR_NO_RESOURCE;
        goto err_free_rkey;
    }

    status = uct_pcie_segment_try_connect_full(rkey->desc->sd,
                                               packed_rkey->node_id,
                                               packed_rkey->seg_id,
                                               &rkey->segment, &rkey->map,
                                               (volatile void**)&rkey->va,
                                               &sci_error);
    if (status == UCS_ERR_BUSY) {
        ucs_error("could not connect to rma segment %u on node %u: %s",
                  packed_rkey->seg_id, packed_rkey->node_id,
                  SCIGetErrorString(sci_error));
        status = UCS_ERR_UNREACHABLE;
        goto err_put_desc;
    } else if (status != UCS_OK) {
        ucs_error("failed to map rma segment %u on node %u",
                  packed_rkey->seg_id, packed_rkey->node_id);
        goto err_put_desc;
    }

    rkey->refcount = 1;
    rkey->node_id  = packed_rkey->node_id;
    rkey->seg_id   = packed_rkey->seg_id;
    rkey->base     = packed_rkey->base;
    ucs_list_add_tail(&uct_pcie_rkeys, &rkey->list);

    ucs_debug("rma segment %u on node %u mapped at %p", rkey->seg_id,
              rkey->node_id, rkey->va);

out:
    *rkey_p   = (uintptr_t)rkey;
    *handle_p = rkey;
out_unlock:
    pthread_mutex_unlock(&uct_pcie_rkey_lock);
    return status;

err_put_desc:
    uct_pcie_rkey_desc_put(rkey->desc);
err_free_rkey:
    ucs_free(rkey);
    goto out_unlock;
}

/* Unmap the segment once its last rkey is released */
static ucs_status_t uct_pcie_md_rkey_release(uct_component_t *component,
                                             uct_rkey_t rkey, void *handle)
{
    uct_pcie_rkey_t *pcie_rkey = handle;

    pthread_mutex_lock(&uct_pcie_rkey_lock);

    if (--pcie_rkey->refcount == 0) {
        ucs_debug("rma segment %u on node %u unmapped", pcie_rkey->seg_id,
                  pcie_rkey->node_id);
        ucs_list_del(&pcie_rkey->list);
        uct_pcie_segment_disconnect(pcie_rkey->segment, pcie_rkey->map);
        uct_pcie_rkey_desc_put(pcie_rkey->desc);
        ucs_free(pcie_rkey);
    }

    pthread_mutex_unlock(&uct_pcie_rkey_lock);
    return UCS_OK;
}

static ucs_status_t
uct_pcie_md_open(uct_component_t *component, const char *md_name,
                 const uct_md_config_t *config, uct_md_h *md_p)
{
    static uct_md_ops_t md_ops = {
        .close     = uct_pcie_md_close,
        .query     = uct_pcie_md_query,
        .mem_alloc = uct_pcie_mem_alloc,
        .mem_free  = uct_pcie_mem_free,
        .mkey_pack = uct_pcie_md_mkey_pack,
        /* SISCI needs a bus address to register memory, not a VA */
        .mem_reg = (uct_md_mem_reg_func_t)ucs_empty_function_return_unsupported,
        .mem_dereg          = (uct_md_mem_dereg_func_t)
                ucs_empty_function_return_unsupported,
        .detect_memory_type = (uct_md_detect_memory_type_func_t)
                ucs_empty_function_return_unsupported
    };
    uct_pcie_md_t *md;
    sci_error_t sci_error;
    ucs_status_t status;

    /* Several contexts can open the MD in one process, so each open gets
     * its own instance */
    md = ucs_malloc(sizeof(*md), "uct_pcie_md_t");
    if (md == NULL) {
        ucs_error("failed to allocate uct_pcie_md_t");
        return UCS_ERR_NO_MEMORY;
    }

    SCIGetLocalNodeId(UCT_PCIE_LOCAL_ADAPTER_NO, &md->node_id,
                      UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIGetLocalNodeId failed: %s", SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_free_md;
    }

    SCIOpen(&md->sci_virtual_device, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen failed: %s", SCIGetErrorString(sci_error));
        status = UCS_ERR_NO_RESOURCE;
        goto err_free_md;
    }

    /* Pre-allocate the shared segment that all mem_alloc calls draw from, and
     * make it available now so remote EPs can connect during the handshake */
    status = uct_pcie_segment_create_available(md->sci_virtual_device,
                                               UCT_PCIE_RMA_SEG_SIZE, NULL,
                                               NULL, &md->rma_seg,
                                               &md->rma_seg_map,
                                               &md->rma_seg_id, &md->rma_buf);
    if (status != UCS_OK) {
        ucs_error("failed to create pcie rma segment");
        goto err_close_vdev;
    }

    md->rma_allocated   = 0;
    md->super.ops       = &md_ops;
    md->super.component = &uct_pcie_component;

    *md_p = &md->super;

    ucs_debug("md %p rma segment %u mapped at %p", md, md->rma_seg_id,
              md->rma_buf);

    return UCS_OK;

err_close_vdev:
    SCIClose(md->sci_virtual_device, UCT_PCIE_NO_FLAGS, &sci_error);
err_free_md:
    ucs_free(md);
    return status;
}

uct_component_t uct_pcie_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_pcie_md_open,
    .cm_open            = (uct_component_cm_open_func_t)
            ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_pcie_md_rkey_unpack,
    .rkey_ptr           = (uct_component_rkey_ptr_func_t)
            ucs_empty_function_return_unsupported,
    .rkey_release       = uct_pcie_md_rkey_release,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = UCT_PCIE_NAME,
    .md_config          = UCT_MD_DEFAULT_CONFIG_INITIALIZER,
    .tl_list     = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_pcie_component),
    .flags       = 0,
    .md_vfs_init = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
UCT_COMPONENT_REGISTER(&uct_pcie_component)
