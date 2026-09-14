#include "laso_plugin.h"
static const laso_plugin_descriptor descriptor = {
    sizeof(laso_plugin_descriptor), 999, "incompatible-example", "0", "Intentional test fixture"};
const laso_plugin_descriptor *laso_plugin_query(void) {
  return &descriptor;
}
int32_t laso_plugin_init(const laso_host_api *host, laso_plugin_handle *out) {
  (void)host;
  (void)out;
  return LASO_FAILED;
}
void laso_plugin_shutdown(laso_plugin_handle handle) {
  (void)handle;
}
