#include <authum/client.h>
#include <fastdl/bridge.h>
AUTHUM_MODULE_EXPORT const authum_module_api_t authum_module_api = {
    .start=fastdl_authum_start, .stop=fastdl_authum_stop
};
