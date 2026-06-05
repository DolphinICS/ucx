
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

    SCIClose(sci_md->sci_virtual_device, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("Error closing Virtual_Device error: %s",
            SCIGetErrorString(sci_error));
    }
}

static ucs_status_t uct_pcie_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    /* UCT_MD_FLAG_NEED_RKEY tells UCX to call mkey_pack after mem_alloc so
     * that the remote side can unpack a rkey and use it for put/get. */
    attr->flags               = UCT_MD_FLAG_ALLOC | UCT_MD_FLAG_NEED_RKEY;
    attr->max_alloc           = 0;
    attr->reg_mem_types       = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->alloc_mem_types     = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->detect_mem_types    = 0;
    attr->max_reg             = ULONG_MAX;
    attr->rkey_packed_size    = sizeof(uct_pcie_rkey_packed_t);
    attr->reg_cost            = ucs_linear_func_make(0, 0);
    memset(&attr->local_cpus, 0xff, sizeof(attr->local_cpus));
    return UCS_OK;
}

/*
 * mem_alloc: allocate memory as a SISCI local segment so that remote nodes
 * can connect to it and write (put) or read (get) via their mapped pointer.
 * The segment is set available immediately so that rkey holders can connect
 * at any time after the rkey is exchanged.
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
    uct_pcie_md_t       *md     = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_mem_handle_t *handle;
    int ret;

    handle = ucs_malloc(sizeof(*handle), "uct_pcie_mem_alloc");
    if (handle == NULL) {
        ucs_error("failed to allocate uct_pcie_mem_handle_t");
        return UCS_ERR_NO_MEMORY;
    }

    handle->length = *length_p;

    ret = uct_pcie_helper_create_seg_set_avail(
        md->sci_virtual_device,
        &handle->segment,
        &handle->segment_map,
        *length_p,
        &handle->segment_id,
        &handle->ptr);
    if (ret != 0) {
        ucs_error("uct_pcie_mem_alloc: failed to create SISCI segment");
        ucs_free(handle);
        return UCS_ERR_NO_RESOURCE;
    }

    *memh_p    = handle;
    *address_p = handle->ptr;
    return UCS_OK;
}

static ucs_status_t uct_pcie_mem_free(uct_md_h md, uct_mem_h memh)
{
    uct_pcie_mem_handle_t *handle = (uct_pcie_mem_handle_t *)memh;
    uct_pcie_helper_remove_seg_set_unavail(handle->segment, handle->segment_map);
    ucs_free(handle);
    return UCS_OK;
}

/*
 * mkey_pack: serialise the information needed to connect to this segment from
 * a remote node into the rkey buffer.  The buffer is exactly
 * sizeof(uct_pcie_rkey_packed_t) bytes (as reported in md_query).
 */
static ucs_status_t uct_pcie_mkey_pack(uct_md_h uct_md, uct_mem_h memh,
                                        void *rkey_buffer, size_t rkey_buffer_size,
                                        const uct_md_mkey_pack_params_t *params,
                                        void *priv)
{
    uct_pcie_md_t         *md     = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_mem_handle_t *handle = (uct_pcie_mem_handle_t *)memh;
    uct_pcie_rkey_packed_t *rkey  = (uct_pcie_rkey_packed_t *)rkey_buffer;

    rkey->segment_id = handle->segment_id;
    rkey->node_id    = md->node_id;
    rkey->base_va    = (uint64_t)(uintptr_t)handle->ptr;
    rkey->length     = handle->length;
    return UCS_OK;
}

/*
 * rkey_unpack: deserialise a packed rkey received from the remote side.
 * We allocate a copy of the packed struct and store a pointer to it in
 * *rkey_p.  The put/get functions in pcie_ep.c cast rkey back to a pointer
 * to find the segment coordinates.  *handle_p receives the same pointer so
 * that rkey_release can free it.
 */
static ucs_status_t uct_pcie_md_rkey_unpack(uct_component_t *component,
    const void *rkey_buffer, uct_rkey_t *rkey_p, void **handle_p)
{
    uct_pcie_rkey_packed_t *rkey = ucs_malloc(sizeof(*rkey), "pcie_rkey");
    if (rkey == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    *rkey    = *(const uct_pcie_rkey_packed_t *)rkey_buffer;
    *rkey_p   = (uct_rkey_t)(uintptr_t)rkey;
    *handle_p = rkey;
    return UCS_OK;
}

static ucs_status_t uct_pcie_rkey_release(uct_component_t *component,
    uct_rkey_t rkey, void *handle)
{
    ucs_free(handle);
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
    unsigned int node_id;

    SCIOpen(&md.sci_virtual_device, 0, &errors);
    if (errors != SCI_ERR_OK) {
        ucs_error("SCIOpen: %s", SCIGetErrorString(errors));
        return UCS_ERR_NO_RESOURCE;
    }

    SCIGetLocalNodeId(UCT_PCIE_LOCAL_ADAPTER_NO, &node_id, 0, &errors);
    if (errors != SCI_ERR_OK) {
        ucs_error("SCIGetLocalNodeId: %s", SCIGetErrorString(errors));
        SCIClose(md.sci_virtual_device, 0, &errors);
        return UCS_ERR_NO_RESOURCE;
    }

    md.super.ops       = &md_ops;
    md.super.component = &uct_pcie_component;
    md.num_devices     = md_config->num_devices;
    md.node_id         = node_id;

    *md_p   = &md.super;
    md_name = "pcie";
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
