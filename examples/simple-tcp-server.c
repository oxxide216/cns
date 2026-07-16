#include <string.h>

#include "cns/cns.h"

#define PORT        2160
#define CLIENTS_MAX 10

static CnsConnection *client_connections[CLIENTS_MAX];
static CnsTimer *client_timers[CLIENTS_MAX];
static unsigned long clients_len = 0;

CnsResult timer_tick(CnsCtx *ctx, CnsTimer *timer) {
  (void) ctx;

  unsigned long client_index = (unsigned long) cns_get_timer_user_data(timer);
  char data[] = "Hello!\n";
  cns_send(client_connections[client_index], (unsigned char *) data, sizeof(data) - 1);

  printf("[INFO] Sent %lu bytes of data to client %s\n",
         sizeof(data) - 1,
         cns_get_connection_address(client_connections[client_index]));

  return CnsResultOk;
}

CnsResult connected(CnsCtx *ctx, CnsConnection *connection) {
  printf("[INFO] New client %s connected\n", cns_get_connection_address(connection));

  if (clients_len >= CLIENTS_MAX) {
    fprintf(stderr, "[ERROR] Not enough room for clients, disconnecting\n");
    return CnsResultNotOk;
  }

  cns_set_connection_user_data(connection, (void *) clients_len);

  client_connections[clients_len] = connection;
  client_timers[clients_len] = cns_start_timer(ctx, 1000, 1000, timer_tick);
  cns_set_timer_user_data(client_timers[clients_len], (void *) clients_len);
  ++clients_len;

  return CnsResultOk;
}

CnsResult data(CnsCtx *ctx, CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  (void) ctx;
  (void) connection;
  (void) data;

  printf("[INFO] Received %lu bytes of data from %s:%u\n",
         data_len, cns_get_connection_address(connection),
         cns_get_connection_port(connection));

  return CnsResultOk;
}

void disconnected(CnsCtx *ctx, CnsConnection *connection) {
  printf("[INFO] Client %s disconnected\n", cns_get_connection_address(connection));

  unsigned long client_index = (unsigned long) cns_get_connection_user_data(connection);

  cns_stop_timer(ctx, client_timers[client_index]);

  --clients_len;
  memmove(client_connections + client_index,
          client_connections + client_index + 1,
          (clients_len - client_index) * sizeof(CnsConnection *));
  memmove(client_timers + client_index,
          client_timers + client_index + 1,
          (clients_len - client_index) * sizeof(CnsTimer *));
}

int main(void) {
  CnsCtx *cns = cns_create();

  CnsListenInfo listen_info = {
    .proto = CnsProtoTCP,
    .receive_timeout = 15,
    .connected_cb = connected,
    .data_cb = data,
    .disconnected_cb = disconnected,
  };
  CnsError error = cns_listen(cns, PORT, &listen_info);
  if (error != CnsErrorOk) {
    fprintf(stderr, "[ERROR] Failed to create server: %s\n", cns_get_error_str(error));
    return 1;
  }

  cns_run(cns);

  return 0;
}
