#include <stdlib.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/un.h>
#include <errno.h>
#include <netdb.h>
#endif

#include "cns/cns.h"
#include "shl/shl-defs.h"
// TODO: also suppress these warnings for clang/msvc
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcalloc-transposed-args"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define TLS_AMALGAMATION
#include "tlse.c"
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

#define MAIN_LOOP_DELAY         1000
#define DEFAULT_DATA_BUFFER_CAP 1024

#ifdef _WIN32
#define MSG_DONTWAIT 0
#define poll WSAPoll

typedef SOCKET Fd;
typedef i32 Size;
typedef char SockOpt;
typedef u_short sa_family_t;
#else
typedef i32 Fd;
typedef u32 Size;
typedef i32 SockOpt;
#endif

struct CnsUdpDest {
  Fd                 fd;
  struct sockaddr_in address_info;
  char               address[256];
};

struct CnsConnection {
  Fd                  fd;
  struct sockaddr_in  address_info;
  CnsProto            proto;
  char                address[256];
  CnsUdpDest          udp_dest;
  struct TLSContext  *tls;
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
  Data                       data_decoded; // TLS only
  CnsConnectedCallback       connected_cb;
  CnsConnectionDataCallback  data_cb;
  CnsDisconnectedCallback    disconnected_cb;
  const char                *unix_socket_path;
  struct TLSContext         *tls;
} Server;

typedef Da(Server) Servers;

typedef struct {
  Fd                         fd;
  unsigned int               receive_timeout;
  bool                       connected;
  CnsConnection              connection;
  Data                       data;
  Data                       data_decoded; // TLS only
  CnsConnectedCallback       connected_cb;
  CnsConnectionDataCallback  data_cb;
  CnsDisconnectedCallback    disconnected_cb;
} TcpUnixTlsClient;

typedef Da(TcpUnixTlsClient) TcpUnixTlsClients;

typedef struct {
  Fd              fd;
  unsigned int    receive_timeout;
  Data            data;
  CnsDataCallback data_cb;
} UdpClient;

struct CnsCtx {
  Servers            servers;
  TcpUnixTlsClients  tcp_unix_tls_clients;
  UdpClient          udp_client;
  CnsTimer          *timers;
  CnsTimer          *timers_end;
  void              *user_data;
  bool               stop;
  u32                tcp_unix_tls_clients_connected;
};

static void close_socket(Fd socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

static bool is_correct_socket(Fd socket) {
#ifdef _WIN32
  return socket != INVALID_SOCKET;
#else
  return socket >= 0;
#endif
}

static void cns_sleep(u64 delay_ms) {
#ifdef _WIN32
  Sleep(delay_ms);
#else
  usleep(delay_ms);
#endif
}

static u64 get_current_time_ms(void) {
#ifdef _WIN32
  return timeGetTime();
#else
  struct timeval current_time;
  gettimeofday(&current_time, NULL);
  return current_time.tv_sec * 1000 + current_time.tv_usec / 1000;
#endif
}

static void make_non_blocking(Fd fd) {
#ifdef _WIN32
  u_long enable = 1;
  ioctlsocket(fd, FIONBIO, &enable);
#else
  fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
}

static bool is_correct_connect_result(i32 result) {
#ifdef _WIN32
  return result == 0 || WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return result == 0 || errno == EINPROGRESS;
#endif
}

CnsCtx *cns_create(void) {
#ifdef _WIN32
  WSADATA wsa_data;
  WSAStartup(MAKEWORD(2,2), &wsa_data);
#endif

  tls_init();

  CnsCtx *ctx = malloc(sizeof(CnsCtx));
  memset(ctx, 0, sizeof(CnsCtx));
  ctx->stop = false;
  return ctx;
}

static void server_destroy(Server *server) {
  CnsConnection *connection = server->connections;
  while (connection) {
    close_socket(connection->fd);
    tls_destroy_context(connection->tls);
    CnsConnection *next = connection->next;
    free(connection);
    connection = next;
  }

  close_socket(server->fd);
  if (server->proto == CnsProtoUnix) {
#ifndef _WIN32
    unlink(server->unix_socket_path);
#endif
  }

  if (server->connection_poll_fds.items)
    free(server->connection_poll_fds.items);
  if (server->data.items)
    free(server->data.items);
  if (server->data_decoded.items)
    free(server->data_decoded.items);

  if (server->proto == CnsProtoTLS)
    tls_destroy_context(server->tls);
}

static void send_pending(Fd sock, struct TLSContext *tls) {
  u32 buffer_cap = 0;
  u32 buffer_index = 0;
  const u8 *buffer = tls_get_write_buffer(tls, &buffer_cap);

  while (buffer && buffer_cap > 0) {
    i32 result = send(sock, (char *) buffer + buffer_index,
                      buffer_cap - buffer_index, 0);
    if (result > 0) {
      buffer_index += result;
    } else if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd poll_fd = { sock, POLLOUT, 0 };
      poll(&poll_fd, 1, 1500);
    } else {
      break;
    }
  }

  if (buffer && buffer_index >= buffer_cap)
    tls_buffer_clear(tls);
}

static void tcp_unix_tls_client_destroy(TcpUnixTlsClient *client) {
  if (client->connection.proto == CnsProtoTLS) {
    tls_close_notify(client->connection.tls);
    send_pending(client->fd, client->connection.tls);
  }
  close_socket(client->fd);
  if (client->connection.proto == CnsProtoTLS)
    tls_destroy_context(client->connection.tls);

  if (client->data.items)
    free(client->data.items);
  if (client->data_decoded.items)
    free(client->data_decoded.items);
}

static void udp_client_destroy(UdpClient *client) {
  close_socket(client->fd);
  client->fd = 0;

  if (client->data.items)
    free(client->data.items);
}

// TODO: Maybe make it actually do something?
static i32 verify_signature(struct TLSContext *ctx, struct TLSCertificate **cert_chain, i32 len) {
  (void) ctx;
  (void) cert_chain;
  (void) len;
  return no_error;
}

static void main_loop_servers_accept_connections(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->servers.len; ++i) {
    Server *server = ctx->servers.items + i;

    if (server->proto == CnsProtoUDP)
      continue;

    struct sockaddr_in address_info;
    Size address_info_size = sizeof(address_info);
    Fd client = accept(server->fd, (struct sockaddr *) &address_info, &address_info_size);
    if (!is_correct_socket(client))
      continue;

    struct TLSContext *tls;
    if (server->proto == CnsProtoTLS) {
      tls = tls_accept(server->tls);

      u8 buffer[512];
      i32 buffer_len = 0;
      while ((buffer_len = recv(client, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0) {
        if (tls_consume_stream(tls, buffer, buffer_len, verify_signature) < 0) {
          close(client);
          tls_destroy_context(tls);
          tls = NULL;
          break;
        }

        send_pending(client, tls);

        if (tls_established(tls) == 1)
          break;
      }

      if (!tls)
        continue;
    }

    CnsConnection *prev_connections = server->connections;
    CnsConnection *prev_connections_end = server->connections_end;
    LL_PREPEND(server->connections, server->connections_end, CnsConnection);

    server->connections_end->fd = client;
    if (server->proto != CnsProtoUnix)
      server->connections_end->address_info = address_info;
    server->connections_end->proto = server->proto;
    server->connections_end->owner_server_index = i;
    server->connections_end->owner_client_index = (u32) -1;
    if (server->proto != CnsProtoUnix)
      inet_ntop(AF_INET, &address_info.sin_addr,
                server->connections_end->address,
                sizeof(server->connections_end->address));
    server->connections_end->tls = tls;
    server->connections_end->next = NULL;

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

static i32 tcp_unix_receive_data(Fd fd, Data *data) {
  return recv(fd, (char *) data->items + data->len,
              data->cap - data->len, MSG_DONTWAIT);
}

static i32 tls_receive_data(Fd fd, struct TLSContext *tls, Data *data) {
  i32 buffer_len = recv(fd, data->items + data->len, data->cap - data->len, MSG_DONTWAIT);
  if (buffer_len <= 0)
    return buffer_len;

  tls_consume_stream(tls,
                     data->items + data->len,
                     buffer_len,
                     verify_signature);

  return buffer_len;
}

static i32 tls_decode_data(struct TLSContext *tls, Data *data) {
  return tls_read(tls, data->items + data->len, data->cap - data->len);
}

static void tcp_unix_tls_server_receive_data(CnsCtx *ctx, Server *server, bool is_tls) {
  Data *data = &server->data;
  Data *data_decoded = &server->data_decoded;

  if (poll(server->connection_poll_fds.items,
           server->connection_poll_fds.len,
           server->receive_timeout) <= 0)
    return;

  CnsConnection **connection = &server->connections;
  u32 i = 0;
  while (*connection && i < server->connection_poll_fds.len) {
    server->connection_poll_fds.items[i].events = POLLIN;

    if (server->connection_poll_fds.items[i].revents & POLLHUP) {
      if (server->disconnected_cb)
        server->disconnected_cb(ctx, *connection);
      close_socket((*connection)->fd);
      tls_destroy_context((*connection)->tls);
      CnsConnection *next = (*connection)->next;
      if (*connection == server->connections_end)
        server->connections_end = next;
      free(*connection);
      *connection = next;
      DA_REMOVE_AT(server->connection_poll_fds, i);
      continue;
    }

    if (server->connection_poll_fds.items[i].revents & POLLIN) {
      i32 len = 0;
      if (is_tls) {
        while ((len = tls_receive_data((*connection)->fd, (*connection)->tls, data)) > 0) {
          if ((u32) len == data->cap - data->len) {
            data->cap *= 2;
            data->items = realloc(data->items, data->cap);
          }
          data->len += len;
        }

        send_pending((*connection)->fd, (*connection)->tls);

        i32 internal_len;
        while ((internal_len = tls_decode_data((*connection)->tls, data_decoded)) > 0) {
          if ((u32) internal_len == data_decoded->cap - data_decoded->len) {
            data_decoded->cap *= 2;
            data_decoded->items = realloc(data_decoded->items, data_decoded->cap);
          }
          data_decoded->len += internal_len;
        }
      } else {
        while ((len = tcp_unix_receive_data((*connection)->fd, data)) > 0) {
          if ((u32) len == data->cap - data->len) {
            data->cap *= 2;
            data->items = realloc(data->items, data->cap);
          }
          data->len += len;
        }
      }

      if (len == 0) {
        if (server->disconnected_cb)
          server->disconnected_cb(ctx, *connection);
        data->len = 0;
        data_decoded->len = 0;
        close_socket((*connection)->fd);
        tls_destroy_context((*connection)->tls);
        CnsConnection *next = (*connection)->next;
        if (*connection == server->connections_end)
          server->connections_end = next;
        free(*connection);
        *connection = next;
        DA_REMOVE_AT(server->connection_poll_fds, i);
        continue;
      }

      Data *d = server->proto == CnsProtoTLS ? data_decoded : data;
      if (d->len > 0 && server->data_cb) {
        if (server->data_cb(ctx, *connection, d->items, d->len) != CnsResultOk) {
          data->len = 0;
          data_decoded->len = 0;
          close_socket((*connection)->fd);
          tls_destroy_context((*connection)->tls);
          CnsConnection *next = (*connection)->next;
          if (*connection == server->connections_end)
            server->connections_end = next;
          free(*connection);
          *connection = next;
          DA_REMOVE_AT(server->connection_poll_fds, i);
          continue;
        }
      }
      data->len = 0;
      data_decoded->len = 0;
    }

    if (*connection) {
      connection = &(*connection)->next;
      ++i;
    }
  }
}

static i32 udp_receive_data(Fd fd, Data *data, struct sockaddr_in *address_info) {
  Size address_info_size = sizeof(*address_info);
  return recvfrom(fd, (char *) data->items + data->len, data->cap - data->len,
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
    case CnsProtoTCP:
    case CnsProtoUnix:
    case CnsProtoTLS: {
      tcp_unix_tls_server_receive_data(ctx, server, server->proto == CnsProtoTLS);
    } break;

    case CnsProtoUDP: {
      udp_server_receive_data(ctx, server, i);
    } break;
    }
  }
}

static void main_loop_clients_confirm_connections(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->tcp_unix_tls_clients.len; ++i) {
    TcpUnixTlsClient *client = ctx->tcp_unix_tls_clients.items + i;

    if (client->connected)
      continue;

    struct pollfd poll_fd = { client->fd, POLLOUT, 0 };

    if (poll(&poll_fd, 1, client->receive_timeout) <= 0)
      continue;

    if (poll_fd.revents & POLLOUT) {
      SockOpt error;
      Size error_len = sizeof(error);
      getsockopt(client->fd, SOL_SOCKET, SO_ERROR, &error, &error_len);

      if (error == 0) {
        if (client->connected_cb) {
          if (client->connected_cb(ctx, &client->connection) != CnsResultOk) {
            tcp_unix_tls_client_destroy(client);
            DA_REMOVE_AT(ctx->tcp_unix_tls_clients, i);
            --i;
          } else {
            client->connected = true;
            ++ctx->tcp_unix_tls_clients_connected;
          }
        } else {
          client->connected = true;
          ++ctx->tcp_unix_tls_clients_connected;
        }
      } else {
        if (client->disconnected_cb)
          client->disconnected_cb(ctx, &client->connection);
        DA_REMOVE_AT(ctx->tcp_unix_tls_clients, i);
        --i;
      }
    }
  }
}

static void main_loop_clients_receive_data(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->tcp_unix_tls_clients.len; ++i) {
    TcpUnixTlsClient *client = ctx->tcp_unix_tls_clients.items + i;
    Data *data = &client->data;
    Data *data_decoded = &client->data_decoded;

    if (!client->connected)
      continue;

    struct pollfd poll_fd = { client->fd, POLLIN, 0 };

    if (poll(&poll_fd, 1, client->receive_timeout) <= 0)
      continue;

    if ((poll_fd.revents & POLLIN) == 0)
      continue;

    i32 len;
    if (client->connection.proto == CnsProtoTLS) {
      while ((len = tls_receive_data(client->fd, client->connection.tls, data)) > 0) {
        if ((u32) len == data->cap - data->len) {
          data->cap *= 2;
          data->items = realloc(data->items, data->cap);
        }
        data->len += len;
      }

      i32 internal_len;
      while ((internal_len = tls_decode_data(client->connection.tls, data_decoded)) > 0) {
        if ((u32) internal_len == data_decoded->cap - data_decoded->len) {
          data_decoded->cap *= 2;
          data_decoded->items = realloc(data_decoded->items, data_decoded->cap);
        }
        data_decoded->len += internal_len;
      }
    } else {
      while ((len = tcp_unix_receive_data(client->fd, data)) > 0) {
        if ((u32) len == data->cap - data->len) {
          data->cap *= 2;
          data->items = realloc(data->items, data->cap);
        }
        data->len += len;
      }
    }

    if (len == 0) {
      if (client->disconnected_cb)
        client->disconnected_cb(ctx, &client->connection);
      data->len = 0;
      data_decoded->len = 0;
      tcp_unix_tls_client_destroy(client);
      DA_REMOVE_AT(ctx->tcp_unix_tls_clients, i);
      --i;
      --ctx->tcp_unix_tls_clients_connected;
      continue;
    }

    Data *d = client->connection.proto == CnsProtoTLS ? data_decoded : data;
    if (d->len > 0 && client->data_cb) {
      if (client->data_cb(ctx, &client->connection, d->items, d->len) != CnsResultOk) {
        data->len = 0;
        data_decoded->len = 0;
        tcp_unix_tls_client_destroy(client);
        DA_REMOVE_AT(ctx->tcp_unix_tls_clients, i);
        --i;
        --ctx->tcp_unix_tls_clients_connected;
        continue;
      }
    }

    data->len = 0;
    data_decoded->len = 0;
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
  u64 current_time_ms = get_current_time_ms();

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
       ctx->tcp_unix_tls_clients.len == 0 &&
       ctx->udp_client.fd == 0 &&
       !ctx->timers))
    return 0;

  main_loop_servers_accept_connections(ctx);
  main_loop_servers_receive_data(ctx);

  if (ctx->tcp_unix_tls_clients_connected < ctx->tcp_unix_tls_clients.len || ctx->udp_client.fd != 0)
    main_loop_clients_confirm_connections(ctx);
  main_loop_clients_receive_data(ctx);

  if (ctx->timers)
    main_loop_timers_update(ctx);

  cns_sleep(delay_ms);

  return !ctx->stop &&
         (ctx->servers.len > 0 ||
          ctx->tcp_unix_tls_clients.len > 0 ||
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
#ifdef _WIN32
  WSACleanup();
#endif

  for (u32 i = 0; i < ctx->servers.len; ++i)
    server_destroy(ctx->servers.items + i);
  if (ctx->servers.items)
    free(ctx->servers.items);

  for (u32 i = 0; i < ctx->tcp_unix_tls_clients.len; ++i)
    tcp_unix_tls_client_destroy(ctx->tcp_unix_tls_clients.items + i);
  if (ctx->tcp_unix_tls_clients.items)
    free(ctx->tcp_unix_tls_clients.items);

  udp_client_destroy(&ctx->udp_client);

  free(ctx);
}

static i32 proto_to_socket_type(CnsProto proto) {
  switch (proto) {
  case CnsProtoTCP:  return SOCK_STREAM;
  case CnsProtoUDP:  return SOCK_DGRAM;
  case CnsProtoUnix: return SOCK_STREAM;
  case CnsProtoTLS:  return SOCK_STREAM;
  }

  return 0;
}

CnsError cns_resolve(const char *host_name, CnsProto proto,
                     char *out_addr, unsigned int out_addr_len) {
  if (proto == CnsProtoUnix)
    return CnsErrorProtoNotSupported;

  struct addrinfo hints = {0};
  hints.ai_family = AF_INET;
  hints.ai_socktype = proto_to_socket_type(proto);

  struct addrinfo *result;
  if (getaddrinfo(host_name, NULL, &hints, &result) != 0)
    return CnsErrorCouldNotResolve;

  struct addrinfo *info = result;
  while (info) {
    struct in_addr *addr = &(((struct sockaddr_in *) info->ai_addr)->sin_addr);
    if (inet_ntop(AF_INET, addr, out_addr, out_addr_len) != NULL) {
      freeaddrinfo(result);
      return CnsErrorOk;
    }

    info = info->ai_next;
  }

  freeaddrinfo(result);

  return CnsErrorCouldNotResolve;
}

CnsError cns_listen(CnsCtx *ctx, unsigned short port, CnsListenInfo *info) {
  SockOpt enable = 1;

  Fd sock = socket(AF_INET, proto_to_socket_type(info->proto), 0);
  if (!is_correct_socket(sock))
    return CnsErrorCouldNotCreateSocket;

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  make_non_blocking(sock);

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *) &address, sizeof(struct sockaddr_in)) < 0) {
    close_socket(sock);
    return CnsErrorCouldNotBind;
  }

  struct TLSContext *tls = NULL;
  if (info->proto == CnsProtoTCP || info->proto == CnsProtoTLS) {
    if (listen(sock, 0) < 0) {
      close_socket(sock);
      return CnsErrorCouldNotListen;
    }
  }

  if (info->proto == CnsProtoTLS) {
    tls = tls_create_context(1, TLS_V12);
    if (tls_load_certificates(tls, info->cert.data, info->cert.len) < 0) {
      close_socket(sock);
      tls_destroy_context(tls);
      return CnsErrorInvalidCertificate;
    }
    if (tls_load_private_key(tls, info->key.data, info->key.len) < 0) {
      close_socket(sock);
      tls_destroy_context(tls);
      return CnsErrorInvalidKey;
    }
  }

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  Data data_decoded = {0};
  if (info->proto == CnsProtoTLS) {
    data_decoded.len = 0;
    data_decoded.cap = DEFAULT_DATA_BUFFER_CAP;
    data_decoded.items = malloc(data_decoded.cap);
  }

  Server server = {
    sock,
    info->proto,
    info->receive_timeout,
    NULL, NULL, {},
    data, data_decoded,
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
    NULL, tls,
  };
  DA_APPEND(ctx->servers, server);

  return CnsErrorOk;
}

CnsError cns_unix_listen(CnsCtx *ctx, const char *path, CnsListenInfo *info) {
  Fd sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (!is_correct_socket(sock))
    return CnsErrorCouldNotCreateSocket;

#ifndef _WIN32
  unlink(path);
#endif
  make_non_blocking(sock);

  struct sockaddr_un *address =
    malloc(sizeof(sa_family_t) +
           offsetof(struct sockaddr_un, sun_path) +
           strlen(path) + 1);
  address->sun_family = AF_UNIX;
  strcpy(address->sun_path, path);

  if (bind(sock, (struct sockaddr *) address, sizeof(struct sockaddr_un)) < 0) {
    close_socket(sock);
    return CnsErrorCouldNotBind;
  }

  free(address);

  if (listen(sock, 0) < 0) {
    close_socket(sock);
    return CnsErrorCouldNotListen;
  }

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  Server server = {
    sock,
    CnsProtoUnix,
    info->receive_timeout,
    NULL, NULL, {},
    data, {},
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
    path, NULL,
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
        if ((*_connection)->proto == CnsProtoTLS) {
          tls_close_notify((*_connection)->tls);
          send_pending((*_connection)->fd, (*_connection)->tls);
        }
        close_socket((*_connection)->fd);
        tls_destroy_context((*_connection)->tls);
        CnsConnection *next = (*_connection)->next;
        if (*_connection == server->connections_end)
          server->connections_end = next;
        free(*_connection);
        *_connection = next;
        DA_REMOVE_AT(server->connection_poll_fds, i);

        break;
      }

      _connection = &(*_connection)->next;
      ++i;
    }
  } else if (connection->owner_client_index != (u32) -1) {
    tcp_unix_tls_client_destroy(ctx->tcp_unix_tls_clients.items + connection->owner_client_index);
    DA_REMOVE_AT(ctx->tcp_unix_tls_clients, connection->owner_client_index);
  }
}

CnsError cns_tcp_connect(CnsCtx *ctx, const char *addr, unsigned short port, CnsTcpConnectInfo *info) {
  Fd sock = socket(AF_INET, SOCK_STREAM, 0);
  if (!is_correct_socket(sock))
    return CnsErrorCouldNotCreateSocket;

  make_non_blocking(sock);

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons(port);

  if (inet_pton(AF_INET, addr, &address.sin_addr) < 0) {
    close_socket(sock);
    return CnsErrorInvalidAddress;
  }

  i32 result = connect(sock, (struct sockaddr *) &address, sizeof(address));
  if (!is_correct_connect_result(result)) {
    close_socket(sock);
    return CnsErrorCouldNotConnect;
  }

  CnsConnection connection = {
    sock,
    address,
    CnsProtoTCP,
    {},
    {},
    NULL,
    NULL,
    (u32) -1,
    ctx->tcp_unix_tls_clients.len,
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

  TcpUnixTlsClient client = {
    sock,
    info->receive_timeout,
    false,
    connection,
    data, {},
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
  };
  DA_APPEND(ctx->tcp_unix_tls_clients, client);

  return CnsErrorOk;
}

bool cns_tcp_send(CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  if (connection->proto != CnsProtoUDP)
    return send(connection->fd, (char *) data, data_len, MSG_DONTWAIT) == (long) data_len;
  return false;
}

CnsError cns_udp_init(CnsCtx *ctx, CnsUdpInitInfo *info) {
  SockOpt enable = 1;

  Fd sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (!is_correct_socket(sock))
    return CnsErrorCouldNotCreateSocket;

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  make_non_blocking(sock);

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

void cns_udp_enable_broadcast_send(CnsCtx *ctx) {
  SockOpt enable = 1;

  for (u32 i = 0; i < ctx->servers.len; ++i)
    if (ctx->servers.items[i].proto == CnsProtoUDP)
      setsockopt(ctx->servers.items[i].fd, SOL_SOCKET,
                 SO_BROADCAST, &enable, sizeof(enable));
  setsockopt(ctx->udp_client.fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
}

void cns_udp_enable_multicast_receive(CnsCtx *ctx, char *group) {
  struct ip_mreq mreq;
  mreq.imr_multiaddr.s_addr = inet_addr(group);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);

  for (u32 i = 0; i < ctx->servers.len; ++i)
    if (ctx->servers.items[i].proto == CnsProtoUDP)
      setsockopt(ctx->servers.items[i].fd, IPPROTO_IP,
                 IP_ADD_MEMBERSHIP, (char *) &mreq, sizeof(mreq));
  setsockopt(ctx->udp_client.fd, IPPROTO_IP,
             IP_ADD_MEMBERSHIP,
             (char *) &mreq, sizeof(mreq));
}

CnsUdpDest *cns_udp_create_dest(CnsCtx *ctx, const char *addr, unsigned short port) {
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

bool cns_udp_send(CnsUdpDest *dest, unsigned char *data, unsigned long data_len) {
  return sendto(dest->fd, (char *) data, data_len, MSG_DONTWAIT,
                (struct sockaddr *) &dest->address_info,
                sizeof(dest->address_info)) == (long) data_len;
}

void cns_udp_destroy_dest(CnsUdpDest *dest) {
  free(dest);
}

CnsError cns_unix_connect(CnsCtx *ctx, const char *path, CnsUnixConnectInfo *info) {
  Fd sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (!is_correct_socket(sock))
    return CnsErrorCouldNotCreateSocket;

  make_non_blocking(sock);

  struct sockaddr_un *address =
    malloc(sizeof(sa_family_t) +
           offsetof(struct sockaddr_un, sun_path) +
           strlen(path) + 1);
  address->sun_family = AF_UNIX;
  strcpy(address->sun_path, path);

  i32 result = connect(sock, (struct sockaddr *) address, sizeof(struct sockaddr_un));
  if (!is_correct_connect_result(result)) {
    close_socket(sock);
    return CnsErrorCouldNotConnect;
  }

  free(address);

  CnsConnection connection = {
    sock,
    {},
    CnsProtoUnix,
    {},
    {},
    NULL,
    NULL,
    (u32) -1,
    ctx->tcp_unix_tls_clients.len,
    NULL,
  };

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  TcpUnixTlsClient client = {
    sock,
    info->receive_timeout,
    false,
    connection,
    data, {},
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
  };
  DA_APPEND(ctx->tcp_unix_tls_clients, client);

  return CnsErrorOk;
}

bool cns_unix_send(CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  if (connection->proto != CnsProtoUDP)
    return send(connection->fd, (char *) data, data_len, MSG_DONTWAIT) == (long) data_len;
  return false;
}

CnsError cns_tls_connect(CnsCtx *ctx, const char *host_name, unsigned short port, CnsTlsConnectInfo *info) {
  Fd sock = socket(AF_INET, SOCK_STREAM, 0);
  if (!is_correct_socket(sock))
    return CnsErrorCouldNotCreateSocket;

  make_non_blocking(sock);

  char addr[512];
  cns_resolve(host_name, CnsProtoTLS, addr, sizeof(addr) - 1);

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons(port);

  if (inet_pton(AF_INET, addr, &address.sin_addr) < 0) {
    close_socket(sock);
    return CnsErrorInvalidAddress;
  }

  i32 result = connect(sock, (struct sockaddr *) &address, sizeof(address));
  if (!is_correct_connect_result(result)) {
    close_socket(sock);
    return CnsErrorCouldNotConnect;
  }

  struct TLSContext *tls = tls_create_context(0, TLS_V12);
  tls_sni_set(tls, host_name);
  tls_client_connect(tls);
  send_pending(sock, tls);

  struct pollfd poll_fd = {0};
  poll_fd.fd = sock;
  poll_fd.events = POLLIN;

  u8 buffer[512];
  while (true) {
    i32 result = poll(&poll_fd, 1, info->receive_timeout * 10);

    if (result < 0) {
      close(sock);
      tls_destroy_context(tls);
      return CnsErrorCouldNotPerformHandshake;
    }

    if (poll_fd.revents == 0) {
      if (tls_established(tls) != 1) {
        close(sock);
        tls_destroy_context(tls);
        return CnsErrorCouldNotPerformHandshake;
      }
      break;
    }

    i32 len = recv(sock, buffer, sizeof(buffer), 0);
    if (len <= 0) {
      close(sock);
      tls_destroy_context(tls);
      return CnsErrorCouldNotPerformHandshake;
    }

    if (tls_consume_stream(tls, buffer, len, verify_signature) < 0) {
      close(sock);
      tls_destroy_context(tls);
      return CnsErrorCouldNotPerformHandshake;
    }

    send_pending(sock, tls);

    if (tls_established(tls) == 1)
      break;
  }

  CnsConnection connection = {
    sock,
    {},
    CnsProtoTLS,
    {},
    {},
    tls,
    NULL,
    (u32) -1,
    ctx->tcp_unix_tls_clients.len,
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

  Data data_decoded;
  data_decoded.len = 0;
  data_decoded.cap = DEFAULT_DATA_BUFFER_CAP;
  data_decoded.items = malloc(data_decoded.cap);

  TcpUnixTlsClient client = {
    sock,
    info->receive_timeout,
    false,
    connection,
    data, data_decoded,
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
  };
  DA_APPEND(ctx->tcp_unix_tls_clients, client);

  return CnsErrorOk;
}

bool cns_tls_send(CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  if (connection->proto == CnsProtoTLS) {
    bool result = tls_write(connection->tls, data, data_len) == (long) data_len;
    send_pending(connection->fd, connection->tls);
    return result;
  }
  return false;
}

CnsTimer *cns_start_timer(CnsCtx *ctx, unsigned long start_timeout_ms,
                          unsigned long repeat_timeout_ms, CnsTimerCallback tick_cb) {
  LL_PREPEND(ctx->timers, ctx->timers_end, CnsTimer);
  *ctx->timers_end = (CnsTimer) {
    get_current_time_ms(),
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
  if (connection->proto != CnsProtoUnix)
    return connection->address;
  return NULL;
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
  case CnsErrorOk:                       return "ok";
  case CnsErrorCouldNotCreateSocket:     return "could not create socket";
  case CnsErrorCouldNotBind:             return "could not bind";
  case CnsErrorCouldNotListen:           return "could not listen";
  case CnsErrorInvalidAddress:           return "invalid address";
  case CnsErrorCouldNotConnect:          return "could not connect";
  case CnsErrorProtoNotSupported:        return "protocol not supported";
  case CnsErrorCouldNotResolve:          return "could not resolve";
  case CnsErrorCouldNotPerformHandshake: return "could not perform handshake";
  case CnsErrorInvalidCertificate:       return "invalid certificate";
  case CnsErrorInvalidKey:               return "invalid key";
  }

  return NULL;
}
