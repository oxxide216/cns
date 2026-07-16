#include "cns/cns.h"

#define SERVER_ADDRESS "127.0.0.1"
#define SERVER_PORT    2160

CnsResult connected(CnsCtx *ctx, CnsConnection *connection) {
  (void) ctx;

  printf("[INFO] Connected to server %s!\n", cns_get_connection_address(connection));

  return CnsResultOk;
}

CnsResult data(CnsCtx *ctx, CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  (void) ctx;
  (void) connection;
  (void) data;

  printf("[INFO] Received %lu bytes of data\n", data_len);

  return CnsResultOk;
}

void disconnected(CnsCtx *ctx, CnsConnection *connection) {
  (void) ctx;

  printf("[INFO] Disconnected from server %s\n", cns_get_connection_address(connection));
}

int main(void) {
  CnsCtx *cns = cns_create();

  CnsConnectInfo connect_info = {
    .proto = CnsProtoTCP,
    .receive_timeout = 15,
    .connected_cb = connected,
    .data_cb = data,
    .disconnected_cb = disconnected,
  };
  CnsError error = cns_connect(cns, SERVER_ADDRESS, SERVER_PORT, &connect_info);
  if (error != CnsErrorOk) {
    fprintf(stderr, "[ERROR] Failed to connect to server: %s\n", cns_get_error_str(error));
    return 1;
  }

  cns_run(cns);

  return 0;
}
