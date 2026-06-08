
#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>

#include "stdio.h"

#include "pcie_iface.h"
#include "pcie_md.h"
#include "pcie_sisci_helper.h"

uct_component_t uct_pcie_component;

#define UCT_PCIE_NAME "pcie"

typedef struct uct_pcie_md_config {
    uct_md_config_t super;
    size_t          num_devices;
} uct_pcie_md_config_t;

static void uct_pcie_md_close(uct_md_h md) {
    uct_pcie_md_t *sci_md = ucs_derived_of(md, uct_pcie_md_t);
    sci_error_t sci_error;

    uct_pcie_helper_remove_seg_set_unavail(sci_md->rseg,
                                           sci_md->rseg_map);

    SCIClose(sci_md->sci_virtual_device, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("Error closing Virtual_Device error: %s",
            SCIGetErrorString(sci_error));
    }
}

static ucs_status_t uct_pcie_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    /* No rkey needed: put/get targets are identified via iface_addr (shared
     * segment ID and base_va exchanged at EP creation time), not via rkeys. */
    attr->flags               = UCT_MD_FLAG_ALLOC;
    attr->max_alloc           = 0;
    attr->reg_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->alloc_mem_types     = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->detect_mem_types    = 0;
    attr->max_reg             = ULONG_MAX;
    attr->rkey_packed_size    = 0;
    attr->reg_cost            = ucs_linear_func_make(0, 0);
    memset(&attr->local_cpus, 0xff, sizeof(attr->local_cpus));
    return UCS_OK;
}

/*
 * mem_alloc: bump-allocate from the MD's shared segment.
 * The segment is pre-created and set available at md_open time.  Remote EPs
 * connect to it during EP handshake using the segment ID and base_va published
 * in uct_pcie_iface_addr_t, so no rkey is needed to perform puts.
 */
static ucs_status_t uct_pcie_mem_alloc(
    uct_md_h uct_md,
    size_t *length_p,
    void **address_p,
    ucs_memory_type_t mem_type,
    unsigned flags,
    const char *alloc_name,
    uct_mem_h *memh_p)
{
    uct_pcie_md_t         *md     = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_mem_handle_t *handle;
    size_t                 offset;
    size_t                 length;

    /* Align to 64-byte cache line so concurrent allocations stay independent. */
    length = (*length_p + 63) & ~(size_t)63;

    handle = ucs_malloc(sizeof(*handle), "uct_pcie_mem_alloc");
    if (handle == NULL) {
        ucs_error("failed to allocate uct_pcie_mem_handle_t");
        return UCS_ERR_NO_MEMORY;
    }

    offset = md->rseg_allocated;
    if (offset + length > UCT_PCIE_RSEG_SIZE) {
        ucs_error("pcie shared segment exhausted (%zu/%d bytes used)",
                  offset + length, UCT_PCIE_RSEG_SIZE);
        ucs_free(handle);
        return UCS_ERR_NO_MEMORY;
    }

    md->rseg_allocated += length;
    handle->ptr    = (uint8_t *)md->rseg_buf + offset;
    handle->length = length;

    ucs_debug("mem_alloc: offset=%zu length=%zu ptr=%p",
              offset, length, handle->ptr);

    *memh_p    = handle;
    *address_p = handle->ptr;
    return UCS_OK;
}

static ucs_status_t uct_pcie_mem_free(uct_md_h md, uct_mem_h memh)
{
    /* The shared segment persists until md_close; only the handle is freed. */
    ucs_free(memh);
    return UCS_OK;
}

/* mkey_pack and rkey_unpack are stubs — rkey_packed_size = 0 so UCX does not
 * call them, but we keep them to satisfy the md_ops vtable. */
static ucs_status_t uct_pcie_mkey_pack(uct_md_h uct_md, uct_mem_h memh,
                                        void *rkey_buffer, size_t rkey_buffer_size,
                                        const uct_md_mkey_pack_params_t *params,
                                        void *priv)
{
    return UCS_OK;
}

static ucs_status_t uct_pcie_md_rkey_unpack(uct_component_t *component,
    const void *rkey_buffer, uct_rkey_t *rkey_p, void **handle_p)
{
    *rkey_p   = 0;
    *handle_p = NULL;
    return UCS_OK;
}

static ucs_status_t uct_pcie_rkey_release(uct_component_t *component,
    uct_rkey_t rkey, void *handle)
{
    return UCS_OK;
}

static ucs_status_t uct_pcie_md_open(
    uct_component_t *component,
    const char *md_name,
    const uct_md_config_t *config,
    uct_md_h *md_p)
{
    uct_pcie_md_config_t *md_config =
        ucs_derived_of(config, uct_pcie_md_config_t);

    static uct_md_ops_t md_ops = {
        .close              = uct_pcie_md_close,
        .query              = uct_pcie_md_query,
        .mem_alloc          = uct_pcie_mem_alloc,
        .mem_free           = uct_pcie_mem_free,
        .mkey_pack          = uct_pcie_mkey_pack,
        .mem_reg            = ucs_empty_function_return_unsupported,
        .mem_dereg          = ucs_empty_function_return_unsupported,
        .detect_memory_type = ucs_empty_function_return_unsupported
    };

    static uct_pcie_md_t md;
    sci_error_t errors;
    int ret;

    SCIOpen(&md.sci_virtual_device, 0, &errors);
    if (errors != SCI_ERR_OK) {
        ucs_error("SCIOpen: %s", SCIGetErrorString(errors));
        return UCS_ERR_NO_RESOURCE;
    }

    /* Pre-allocate the shared segment that all mem_alloc calls draw from.
     * Set it available immediately so remote EPs can connect during handshake. */
    ret = uct_pcie_helper_create_seg_set_avail(
        md.sci_virtual_device,
        &md.rseg,
        &md.rseg_map,
        UCT_PCIE_RSEG_SIZE,
        &md.rseg_id,
        &md.rseg_buf);
    if (ret != 0) {
        ucs_error("pcie MD: failed to create shared segment");
        SCIClose(md.sci_virtual_device, 0, &errors);
        return UCS_ERR_NO_RESOURCE;
    }

    md.rseg_allocated  = 0;
    md.super.ops       = &md_ops;
    md.super.component = &uct_pcie_component;
    md.num_devices     = md_config->num_devices;

    *md_p   = &md.super;
    md_name = "pcie";

    ucs_debug("MD open: rseg_id=%u rseg_buf=%p",
              md.rseg_id, md.rseg_buf);

    return UCS_OK;
}

uct_component_t uct_pcie_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_pcie_md_open,
    .cm_open            = ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_pcie_md_rkey_unpack,
    .rkey_ptr           = ucs_empty_function_return_unsupported,
    .rkey_release       = uct_pcie_rkey_release,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = UCT_PCIE_NAME,
    .md_config          = UCT_MD_DEFAULT_CONFIG_INITIALIZER,
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_pcie_component),
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
UCT_COMPONENT_REGISTER(&uct_pcie_component)
