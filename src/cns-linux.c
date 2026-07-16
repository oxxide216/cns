#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/time.h>

#include "cns/cns.h"
#include "shl/shl-defs.h"

#define MAIN_LOOP_DELAY         1000
#define DEFAULT_DATA_BUFFER_CAP 1024

typedef i32 Fd;

struct CnsUdpDest {
  Fd                 fd;
  struct sockaddr_in address_info;
  char               address[INET_ADDRSTRLEN];
};

struct CnsConnection {
  Fd                  fd;
  struct sockaddr_in  address_info;
  CnsProto            proto;
  char                address[INET_ADDRSTRLEN];
  CnsUdpDest          udp_dest;
  void               *user_data;
  u32                 owner_server_index;
  u32                 owner_client_index;
  CnsConnection      *next;
};

struct CnsTimer {
  u64               creation_time_ms;
  u64               last_tick_time_ms;
  u64               start_timeout_ms;
  u64               repeat_timeout_ms;
  void             *user_data;
  CnsTimerCallback  tick_cb;
  CnsTimer         *next;
};

typedef Da(struct pollfd) PollFds;

typedef Da(u8) Data;

typedef struct {
  Fd                         fd;
  CnsProto                   proto;
  unsigned int               receive_timeout;
  CnsConnection             *connections;
  CnsConnection             *connections_end;
  PollFds                    connection_poll_fds;
  Data                       data;
  CnsConnectedCallback       connected_cb;
  CnsConnectionDataCallback  data_cb;
  CnsDisconnectedCallback    disconnected_cb;
} Server;

typedef Da(Server) Servers;

typedef struct {
  Fd                        fd;
  unsigned int              receive_timeout;
  bool                      connected;
  CnsConnection             connection;
  Data                      data;
  CnsConnectedCallback      connected_cb;
  CnsConnectionDataCallback data_cb;
  CnsDisconnectedCallback   disconnected_cb;
} TcpClient;

typedef Da(TcpClient) TcpClients;

typedef struct {
  Fd              fd;
  unsigned int    receive_timeout;
  Data            data;
  CnsDataCallback data_cb;
} UdpClient;

struct CnsCtx {
  Servers     servers;
  TcpClients  tcp_clients;
  UdpClient   udp_client;
  CnsTimer   *timers;
  CnsTimer   *timers_end;
  void       *user_data;
  bool        stop;
  u32         tcp_clients_connected;
};

CnsCtx *cns_create(void) {
  CnsCtx *ctx = malloc(sizeof(CnsCtx));
  memset(ctx, 0, sizeof(CnsCtx));
  ctx->stop = false;
  return ctx;
}

static void server_destroy(Server *server) {
  CnsConnection *connection = server->connections;
  while (connection) {
    close(connection->fd);
    CnsConnection *next = connection->next;
    free(connection);
    connection = next;
  }

  close(server->fd);

  if (server->connection_poll_fds.items)
    free(server->connection_poll_fds.items);
  if (server->data.items)
    free(server->data.items);
}

static void tcp_client_destroy(TcpClient *client) {
  close(client->fd);

  if (client->data.items)
    free(client->data.items);
}

static void udp_client_destroy(UdpClient *client) {
  close(client->fd);
  client->fd = 0;

  if (client->data.items)
    free(client->data.items);
}

static void main_loop_servers_accept_connections(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->servers.len; ++i) {
    Server *server = ctx->servers.items + i;

    if (server->proto == CnsProtoUDP)
      continue;

    struct sockaddr_in address_info;
    u32 address_info_size = sizeof(address_info);
    Fd client = accept(server->fd, (struct sockaddr *) &address_info, &address_info_size);
    if (client < 0)
      continue;

    CnsConnection *prev_connections = server->connections;
    CnsConnection *prev_connections_end = server->connections_end;
    LL_PREPEND(server->connections, server->connections_end, CnsConnection);

    server->connections_end->fd = client;
    server->connections_end->address_info = address_info;
    server->connections_end->proto = server->proto;
    server->connections_end->owner_server_index = i;
    server->connections_end->owner_client_index = (u32) -1;
    inet_ntop(AF_INET, &address_info.sin_addr,
              server->connections_end->address,
              sizeof(server->connections_end->address));

    if (server->connected_cb) {
      if (server->connected_cb(ctx, server->connections_end) != CnsResultOk) {
        free(server->connections_end);
        server->connections = prev_connections;
        server->connections_end = prev_connections_end;
      }
    }

    struct pollfd poll_fd = { server->connections_end->fd, POLLIN, 0 };
    DA_APPEND(server->connection_poll_fds, poll_fd);
  }
}

static i32 tcp_receive_data(Fd fd, Data *data) {
  return recv(fd, data->items + data->len,
              data->cap - data->len, MSG_DONTWAIT);
}

static void tcp_server_receive_data(CnsCtx *ctx, Server *server) {
  Data *data = &server->data;

  if (poll(server->connection_poll_fds.items,
           server->connection_poll_fds.len,
           server->receive_timeout) <= 0)
    return;

  CnsConnection **connection = &server->connections;
  u32 i = 0;
  while (*connection && i < server->connection_poll_fds.len) {
    server->connection_poll_fds.items[i].events = POLLIN;

    if (server->connection_poll_fds.items[i].revents & POLLIN) {
      i32 len = 0;
      while ((len = tcp_receive_data((*connection)->fd, data)) > 0) {
        if ((u32) len == data->cap - data->len) {
          data->cap *= 2;
          data->items = realloc(data->items, data->cap);
        }
        data->len += len;
      }

      if (len == 0) {
        if (server->disconnected_cb)
          server->disconnected_cb(ctx, *connection);
        data->len = 0;
        CnsConnection *next = (*connection)->next;
        free(*connection);
        *connection = next;
        DA_REMOVE_AT(server->connection_poll_fds, i);
        continue;
      }

      if (data->len > 0 && server->data_cb) {
        if (server->data_cb(ctx, *connection, data->items, data->len) != CnsResultOk) {
          data->len = 0;
          close((*connection)->fd);
          CnsConnection *next = (*connection)->next;
          free(*connection);
          *connection = next;
          DA_REMOVE_AT(server->connection_poll_fds, i);
          continue;
        }
      }

      data->len = 0;
    }

    connection = &(*connection)->next;
    ++i;
  }
}

static i32 udp_receive_data(Fd fd, Data *data, struct sockaddr_in *address_info) {
  u32 address_info_size = sizeof(*address_info);
  return recvfrom(fd, data->items + data->len, data->cap - data->len,
                  MSG_DONTWAIT, (struct sockaddr *) address_info, &address_info_size);
}

static bool sockaddr_in_eq(struct sockaddr_in *a, struct sockaddr_in *b) {
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static CnsConnection *get_connection_with_address_info(CnsConnection *connections,
                                                       struct sockaddr_in *address_info) {
  CnsConnection *connection = connections;
  while (connection) {
    if (sockaddr_in_eq(address_info, &connection->address_info))
      return connection;

    connection = connection->next;
  }

  return NULL;
}

static CnsConnection *udp_accept_connection_if_new(CnsCtx *ctx, Server *server,
                                                   u32 server_index,
                                                   struct sockaddr_in *address_info) {
  CnsConnection *connection = get_connection_with_address_info(server->connections, address_info);
  if (connection)
    return connection;

  LL_PREPEND(server->connections, server->connections_end, CnsConnection);

  server->connections_end->fd = server->fd;
  server->connections_end->address_info = *address_info;
  server->connections_end->proto = server->proto;
  server->connections_end->udp_dest.fd = server->fd;
  server->connections_end->udp_dest.address_info = *address_info;
  server->connections_end->owner_server_index = server_index;
  server->connections_end->owner_client_index = (u32) -1;
  inet_ntop(AF_INET, &address_info->sin_addr,
            server->connections_end->address,
            sizeof(server->connections_end->address));
  memcpy(server->connections_end->udp_dest.address,
         server->connections_end->address,
         sizeof(server->connections_end->address));

  if (server->connected_cb)
    server->connected_cb(ctx, server->connections_end);

  return server->connections_end;
}

static void udp_server_receive_data(CnsCtx *ctx, Server *server, u32 index) {
  Data *data = &server->data;

  struct pollfd poll_fd = { server->fd, POLLIN, 0 };

  if (poll(&poll_fd, 1, server->receive_timeout) <= 0)
    return;

  struct sockaddr_in address_info;
  CnsConnection *connection = NULL;
  CnsConnection *prev_connection = NULL;

  i32 len;
  while ((len = udp_receive_data(server->fd, data, &address_info)) > 0) {
    connection = udp_accept_connection_if_new(ctx, server, index, &address_info);
    if (connection != prev_connection) {
      if (data->len > 0) {
        if (server->data_cb)
          server->data_cb(ctx, prev_connection, data->items, data->len);
        memmove(data->items, data->items + data->len, len);
        data->len = 0;
      }
      prev_connection = connection;
    }

    if ((u32) len == data->cap - data->len) {
      data->cap *= 2;
      data->items = realloc(data->items, data->cap);
    }
    data->len += len;
  }

  if (connection && data->len > 0 && server->data_cb)
    server->data_cb(ctx, connection, data->items, data->len);

  data->len = 0;
}

static void main_loop_servers_receive_data(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->servers.len; ++i) {
    Server *server = ctx->servers.items + i;

    switch (server->proto) {
    case CnsProtoTCP: {
      tcp_server_receive_data(ctx, server);
    } break;

    case CnsProtoUDP: {
      udp_server_receive_data(ctx, server, i);
    } break;
    }
  }
}

static void main_loop_clients_confirm_connections(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->tcp_clients.len; ++i) {
    TcpClient *client = ctx->tcp_clients.items + i;

    if (client->connected)
      continue;

    struct pollfd poll_fd = { client->fd, POLLOUT, 0 };

    if (poll(&poll_fd, 1, client->receive_timeout) <= 0)
      continue;

    if (poll_fd.revents & POLLOUT) {
      i32 error;
      u32 error_len = sizeof(error);
      getsockopt(client->fd, SOL_SOCKET, SO_ERROR, &error, &error_len);

      if (error == 0) {
        if (client->connected_cb) {
          if (client->connected_cb(ctx, &client->connection) != CnsResultOk) {
            tcp_client_destroy(client);
            DA_REMOVE_AT(ctx->tcp_clients, i);
            --i;
          } else {
            client->connected = true;
            ++ctx->tcp_clients_connected;
          }
        } else {
          client->connected = true;
          ++ctx->tcp_clients_connected;
        }
      } else {
        if (client->disconnected_cb)
          client->disconnected_cb(ctx, &client->connection);
        DA_REMOVE_AT(ctx->tcp_clients, i);
        --i;
      }
    }
  }
}

static void main_loop_clients_receive_data(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->tcp_clients.len; ++i) {
    TcpClient *client = ctx->tcp_clients.items + i;
    Data *data = &client->data;

    if (!client->connected)
      continue;

    struct pollfd poll_fd = { client->fd, POLLIN, 0 };

    if (poll(&poll_fd, 1, client->receive_timeout) <= 0)
      continue;

    if ((poll_fd.revents & POLLIN) == 0)
      continue;

    i32 len;
    while ((len = tcp_receive_data(client->fd, data)) > 0) {
      if ((u32) len == data->cap - data->len) {
        data->cap *= 2;
        data->items = realloc(data->items, data->cap);
      }
      data->len += len;
    }

    if (len == 0) {
      if (client->disconnected_cb)
        client->disconnected_cb(ctx, &client->connection);
      tcp_client_destroy(client);
      DA_REMOVE_AT(ctx->tcp_clients, i);
      --i;
      --ctx->tcp_clients_connected;
      continue;
    }

    if (data->len > 0 && client->data_cb) {
      if (client->data_cb(ctx, &client->connection, data->items, data->len) != CnsResultOk) {
        tcp_client_destroy(client);
        DA_REMOVE_AT(ctx->tcp_clients, i);
        --i;
        --ctx->tcp_clients_connected;
        continue;
      }
    }

    data->len = 0;
  }

  Data *data = &ctx->udp_client.data;

  i32 len;
  while ((len = udp_receive_data(ctx->udp_client.fd, data, NULL)) > 0) {
    if ((u32) len == data->cap - data->len) {
      data->cap *= 2;
      data->items = realloc(data->items, data->cap);
    }
    data->len += len;
  }

  if (data->len > 0 && ctx->udp_client.data_cb)
    if (ctx->udp_client.data_cb(ctx, data->items, data->len) != CnsResultOk)
      udp_client_destroy(&ctx->udp_client);

  data->len = 0;
}

void main_loop_timers_update(CnsCtx *ctx) {
  struct timeval current_time;
  gettimeofday(&current_time, NULL);
  u64 current_time_ms = current_time.tv_sec * 1000 + current_time.tv_usec / 1000;

  CnsTimer **timer = &ctx->timers;
  while (*timer) {
    bool stopped = false;
    if ((*timer)->last_tick_time_ms == 0) {
      u64 diff = current_time_ms - (*timer)->creation_time_ms;
      if (diff >= (*timer)->start_timeout_ms) {
        if ((*timer)->tick_cb)
          if ((*timer)->tick_cb(ctx, *timer) != CnsResultOk)
            stopped = true;
        (*timer)->last_tick_time_ms = current_time_ms;
        if ((*timer)->repeat_timeout_ms == 0)
          stopped = true;
      }
    } else {
      u64 diff = current_time_ms - (*timer)->last_tick_time_ms;
      if (diff >= (*timer)->repeat_timeout_ms) {
        if ((*timer)->tick_cb)
          if ((*timer)->tick_cb(ctx, *timer) != CnsResultOk)
            stopped = true;
        (*timer)->last_tick_time_ms = current_time_ms;
      }
    }

    if (stopped)
      *timer = (*timer)->next;
    else
      timer = &(*timer)->next;
  }
}

int cns_step(CnsCtx *ctx, unsigned int delay_ms) {
  if (ctx->stop ||
      (ctx->servers.len == 0 &&
       ctx->tcp_clients.len == 0 &&
       ctx->udp_client.fd == 0 &&
       !ctx->timers))
    return 0;

  main_loop_servers_accept_connections(ctx);
  main_loop_servers_receive_data(ctx);

  if (ctx->tcp_clients_connected < ctx->tcp_clients.len || ctx->udp_client.fd != 0)
    main_loop_clients_confirm_connections(ctx);
  main_loop_clients_receive_data(ctx);

  if (ctx->timers)
    main_loop_timers_update(ctx);

  usleep(delay_ms);

  return !ctx->stop &&
         (ctx->servers.len > 0 ||
          ctx->tcp_clients.len > 0 ||
          ctx->udp_client.fd != 0 ||
          ctx->timers);
}

void cns_run(CnsCtx *ctx) {
  while (cns_step(ctx, MAIN_LOOP_DELAY));
}

void cns_stop(CnsCtx *ctx) {
  ctx->stop = true;
}

void cns_destroy(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->servers.len; ++i)
    server_destroy(ctx->servers.items + i);
  if (ctx->servers.items)
    free(ctx->servers.items);

  for (u32 i = 0; i < ctx->tcp_clients.len; ++i)
    tcp_client_destroy(ctx->tcp_clients.items + i);
  if (ctx->tcp_clients.items)
    free(ctx->tcp_clients.items);

  udp_client_destroy(&ctx->udp_client);

  free(ctx);
}

static i32 proto_to_socket_type(CnsProto proto) {
  switch (proto) {
  case CnsProtoTCP: return SOCK_STREAM;
  case CnsProtoUDP: return SOCK_DGRAM;
  }

  return 0;
}

CnsError cns_listen(CnsCtx *ctx, unsigned short port, CnsListenInfo *info) {
  i32 enable = 1;

  Fd sock = socket(AF_INET, proto_to_socket_type(info->proto), 0);
  if (sock < 0)
    return CnsErrorCouldNotCreateSocket;

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  fcntl(sock, F_SETFL, O_NONBLOCK);

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *) &address, sizeof(address)) < 0) {
    close(sock);
    return CnsErrorCouldNotBind;
  }

  if (info->proto == CnsProtoTCP) {
    if (listen(sock, 0) < 0) {
      close(sock);
      return CnsErrorCouldNotListen;
    }
  }

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  Server server = {
    sock,
    info->proto,
    info->receive_timeout,
    NULL, NULL, {}, data,
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
  };
  DA_APPEND(ctx->servers, server);

  return CnsErrorOk;
}

void cns_close(CnsCtx *ctx, CnsConnection *connection) {
  if (connection->owner_server_index != (u32) -1) {
    Server *server = ctx->servers.items + connection->owner_server_index;

    CnsConnection **_connection = &server->connections;
    u32 i = 0;
    while (*_connection && i < server->connection_poll_fds.len) {
      if (*_connection == connection) {
        close((*_connection)->fd);
        CnsConnection *next = (*_connection)->next;
        free(*_connection);
        *_connection = next;
        DA_REMOVE_AT(server->connection_poll_fds, i);

        break;
      }

      _connection = &(*_connection)->next;
      ++i;
    }
  } else if (connection->owner_client_index != (u32) -1) {
    tcp_client_destroy(ctx->tcp_clients.items + connection->owner_client_index);
    DA_REMOVE_AT(ctx->tcp_clients, connection->owner_client_index);
  }
}

CnsError cns_tcp_connect(CnsCtx *ctx, char *addr, unsigned short port, CnsTcpConnectInfo *info) {
  i32 enable = 1;

  Fd sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return CnsErrorCouldNotCreateSocket;

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  fcntl(sock, F_SETFL, O_NONBLOCK);

  struct sockaddr_in address_info;
  address_info.sin_family = AF_INET;
  address_info.sin_port = htons(port);

  if (inet_pton(AF_INET, addr, &address_info.sin_addr) < 0) {
    close(sock);
    return CnsErrorInvalidAddress;
  }

  i32 result = connect(sock, (struct sockaddr *) &address_info, sizeof(address_info));
  if (result != 0 && errno != EINPROGRESS) {
    close(sock);
    return CnsErrorCouldNotConnect;
  }

  CnsConnection connection = {
    sock,
    address_info,
    CnsProtoTCP,
    {},
    {},
    NULL,
    (u32) -1,
    ctx->tcp_clients.len,
    NULL,
  };
  u32 addr_len = strlen(addr);
  if (addr_len > sizeof(connection.address))
    addr_len = sizeof(connection.address);
  memcpy(connection.address, addr, addr_len);

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  TcpClient client = {
    sock,
    info->receive_timeout,
    false,
    connection,
    data,
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
  };
  DA_APPEND(ctx->tcp_clients, client);

  return CnsErrorOk;
}

void cns_tcp_send(CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  if (connection->proto == CnsProtoTCP)
    send(connection->fd, data, data_len, MSG_DONTWAIT);
}

CnsError cns_udp_init(CnsCtx *ctx, CnsUdpInitInfo *info) {
  i32 enable = 1;

  Fd sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    return CnsErrorCouldNotCreateSocket;

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  fcntl(sock, F_SETFL, O_NONBLOCK);

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  ctx->udp_client = (UdpClient) {
    sock,
    info->receive_timeout,
    data,
    info->data_cb,
  };

  return CnsErrorOk;
}

CnsUdpDest *cns_udp_create_dest(CnsCtx *ctx, char *addr, unsigned short port) {
  if (ctx->udp_client.fd == 0)
    return NULL;

  struct sockaddr_in address_info;
  address_info.sin_family = AF_INET;
  address_info.sin_port = htons(port);

  if (inet_pton(AF_INET, addr, &address_info.sin_addr) < 0)
    return NULL;

  CnsUdpDest *dest = malloc(sizeof(CnsUdpDest));
  dest->fd = ctx->udp_client.fd;
  dest->address_info = address_info;
  u32 addr_len = strlen(addr);
  if (addr_len > sizeof(dest->address))
    addr_len = sizeof(dest->address);
  memcpy(dest->address, addr, addr_len);

  return dest;
}

void cns_udp_send(CnsUdpDest *dest, unsigned char *data, unsigned long data_len) {
  sendto(dest->fd, data, data_len, MSG_DONTWAIT,
         (struct sockaddr *) &dest->address_info,
         sizeof(dest->address_info));
}

void cns_udp_destroy_dest(CnsUdpDest *dest) {
  free(dest);
}

CnsTimer *cns_start_timer(CnsCtx *ctx, unsigned long start_timeout_ms,
                          unsigned long repeat_timeout_ms, CnsTimerCallback tick_cb) {
  struct timeval current_time;
  gettimeofday(&current_time, NULL);

  LL_PREPEND(ctx->timers, ctx->timers_end, CnsTimer);
  *ctx->timers_end = (CnsTimer) {
    current_time.tv_sec * 1000 + current_time.tv_usec / 1000,
    0,
    start_timeout_ms,
    repeat_timeout_ms,
    NULL,
    tick_cb,
    NULL,
  };

  return ctx->timers_end;
}

void cns_stop_timer(CnsCtx *ctx, CnsTimer *timer) {
  CnsTimer **_timer = &ctx->timers;
  while (*_timer) {
    if (*_timer == timer) {
      *_timer = (*_timer)->next;
      if (!ctx->timers)
        ctx->timers_end = NULL;
      return;
    }

    _timer = &(*_timer)->next;
  }
}

void *cns_get_user_data(CnsCtx *ctx) {
  return ctx->user_data;
}

void cns_set_user_data(CnsCtx *ctx, void *user_data) {
  ctx->user_data = user_data;
}

void *cns_get_connection_user_data(CnsConnection *connection) {
  return connection->user_data;
}

void cns_set_connection_user_data(CnsConnection *connection, void *user_data) {
  connection->user_data = user_data;
}

char *cns_get_connection_address(CnsConnection *connection) {
  return connection->address;
}

unsigned short cns_get_connection_port(CnsConnection *connection) {
  return ntohs(connection->address_info.sin_port);
}

void *cns_get_timer_user_data(CnsTimer *timer) {
  return timer->user_data;
}

void cns_set_timer_user_data(CnsTimer *timer, void *user_data) {
  timer->user_data = user_data;
}

CnsUdpDest *cns_udp_get_connection_dest(CnsConnection *connection) {
  if (connection->proto == CnsProtoUDP)
    return &connection->udp_dest;
  return NULL;
}

char *cns_udp_get_dest_address(CnsUdpDest *dest) {
  return dest->address;
}

unsigned short cns_udp_get_dest_port(CnsUdpDest *dest) {
  return ntohs(dest->address_info.sin_port);
}

char *cns_get_error_str(CnsError error) {
  switch (error) {
  case CnsErrorOk:                   return "ok";
  case CnsErrorCouldNotCreateSocket: return "could not create socket";
  case CnsErrorCouldNotBind:         return "could not bind";
  case CnsErrorCouldNotListen:       return "could not listen";
  case CnsErrorInvalidAddress:       return "invalid address";
  case CnsErrorCouldNotConnect:      return "could not connect";
  }

  return NULL;
}

void cns_make_non_blocking(FILE *stream) {
  fcntl(fileno(stream), F_SETFL, O_NONBLOCK);
}
