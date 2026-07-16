#include "cns/cns.h"

#define PORT 2160

CnsResult data(CnsCtx *ctx, CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  (void) ctx;

  printf("[INFO] Received %lu bytes of data from %s:%u, sending back\n",
         data_len, cns_get_connection_address(connection),
         cns_get_connection_port(connection));

  cns_udp_send(cns_udp_get_connection_dest(connection), data, data_len);

  return CnsResultOk;
}

int main(void) {
  CnsCtx *cns = cns_create();

  CnsListenInfo listen_info = {
    .proto = CnsProtoUDP,
    .receive_timeout = 15,
    .data_cb = data,
  };
  CnsError error = cns_listen(cns, PORT, &listen_info);
  if (error != CnsErrorOk) {
    fprintf(stderr, "[ERROR] Failed to create server: %s\n", cns_get_error_str(error));
    return 1;
  }

  cns_run(cns);

  cns_destroy(cns);

  return 0;
}
