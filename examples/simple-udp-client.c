#include "cns/cns.h"

#define SERVER_ADDRESS "127.0.0.1"
#define SERVER_PORT    2160

static CnsUdpDest *dest = NULL;

CnsResult timer_tick(CnsCtx *ctx, CnsTimer *timer) {
  (void) ctx;
  (void) timer;

  char data[] = "Hello!\n";
  cns_udp_send(dest, (unsigned char *) data, sizeof(data) - 1);

  printf("[INFO] Sent %lu bytes of data to server %s:%u\n",
         sizeof(data) - 1,
         cns_udp_get_dest_address(dest),
         cns_udp_get_dest_port(dest));

  return CnsResultOk;
}

CnsResult data(CnsCtx *ctx, unsigned char *data, unsigned long data_len) {
  (void) ctx;
  (void) data;

  printf("[INFO] Received %lu bytes of data\n", data_len);

  return CnsResultOk;
}

int main(void) {
  CnsCtx *cns = cns_create();

  CnsUdpInitInfo connect_info = {
    .receive_timeout = 15,
    .data_cb = data,
  };
  CnsError error = cns_udp_init(cns, &connect_info);
  if (error != CnsErrorOk) {
    fprintf(stderr, "[ERROR] Failed to connect to server: %s\n", cns_get_error_str(error));
    return 1;
  }

  dest = cns_udp_create_dest(cns, SERVER_ADDRESS, SERVER_PORT);

  cns_start_timer(cns, 1000, 1000, timer_tick);

  cns_run(cns);

  cns_udp_destroy_dest(dest);
  cns_destroy(cns);

  return 0;
}
