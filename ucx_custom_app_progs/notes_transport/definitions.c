

typedef struct uct_priv_worker {
    uct_worker_t           super;
    ucs_async_context_t    *async;
    ucs_thread_mode_t      thread_mode;
    ucs_list_link_t        tl_data;
} uct_priv_worker_t;
