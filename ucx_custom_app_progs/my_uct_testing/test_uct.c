#include <uct/api/uct.h>
#include <assert.h>
#include <stdlib.h>

#define ERROR_CHECK_UCS_OK(func_name, error) \
if (error != UCS_OK) {  \
    fprintf(stderr, "Error: %s failed, error code %d", func_name, (int)error); \
    exit(EXIT_FAILURE); \
}

int main(int argc, char **argv)
{
    ucs_async_context_t *async;
    uct_worker_h worker;

    /* Initialize context */
    ucs_status_t status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async);
    ERROR_CHECK_UCS_OK("ucs_async_context_create", status)

    /* Create a worker object */
    status = uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker);
    ERROR_CHECK_UCS_OK("ucs_async_context_create", status)

    // /* Search for the desired transport */
    // status = dev_tl_lookup(&cmd_args, &if_info);
    // assert(status == UCS_OK);

    /* Cleanup */
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
    return 0;
}
