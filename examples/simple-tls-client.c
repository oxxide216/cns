#include <stdio.h>
#include <string.h>

#include "cns/cns.h"

#define SERVER_ADDRESS "www.google.com"
#define SERVER_PORT    443

char *get =
  "GET / HTTP/1.1\r\n"
  "Host: " SERVER_ADDRESS "\r\n\r\n";

CnsResult connected(CnsCtx *ctx, CnsConnection *connection) {
  (void) ctx;

  printf("[INFO] Connected to server %s!\n", cns_get_connection_address(connection));

  cns_tls_send(connection, (unsigned char *) get, strlen(get));

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

  CnsTlsConnectInfo connect_info = {
    .receive_timeout = 450,
    .connected_cb = connected,
    .data_cb = data,
    .disconnected_cb = disconnected,
  };
  CnsError error = cns_tls_connect(cns, SERVER_ADDRESS, SERVER_PORT, &connect_info);
  if (error != CnsErrorOk) {
    fprintf(stderr, "[ERROR] Failed to connect to server: %s\n", cns_get_error_str(error));
    return 1;
  }

  cns_run(cns);

  cns_destroy(cns);

  return 0;
}
