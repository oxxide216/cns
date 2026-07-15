#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#include "cns/cns.h"
#include "shl/shl-defs.h"

#define MAIN_LOOP_DELAY         1000
#define DEFAULT_DATA_BUFFER_CAP 1024

typedef i32 Fd;

struct CnsConnection {
  Fd             fd;
  char           address[INET_ADDRSTRLEN];
  void          *user_data;
  u32            owner_server_index;
  u32            owner_client_index;
  CnsConnection *next;
};

typedef Da(struct pollfd) PollFds;

typedef Da(u8) Data;

typedef struct {
  Fd                       fd;
  CnsProto                 proto;
  unsigned int             receive_timeout;
  CnsConnection           *connections;
  CnsConnection           *connections_end;
  PollFds                  connection_poll_fds;
  Data                     data;
  CnsConnectedCallback     connected_cb;
  CnsDataCallback          data_cb;
  CnsDisconnectedCallback  disconnected_cb;
} Server;

typedef Da(Server) Servers;

typedef struct {
  Fd                       fd;
  CnsProto                 proto;
  unsigned int             receive_timeout;
  bool                     connected;
  CnsConnection            connection;
  Data                     data;
  CnsConnectedCallback     connected_cb;
  CnsDataCallback          data_cb;
  CnsDisconnectedCallback  disconnected_cb;
} Client;

typedef Da(Client) Clients;

struct CnsCtx {
  Servers  servers;
  Clients  clients;
  void    *user_data;
  bool     stop;
  u32      clients_connected;
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

static void client_destroy(Client *client) {
  close(client->fd);

  if (client->data.items)
    free(client->data.items);
}

static void main_loop_servers_accept_connections(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->servers.len; ++i) {
    Server *server = ctx->servers.items + i;

    struct sockaddr_in address;
    u32 address_size = sizeof(address);
    Fd client = accept(server->fd, (struct sockaddr *) &address, &address_size);
    if (client < 0)
      continue;

    CnsConnection *prev_connections = server->connections;
    CnsConnection *prev_connections_end = server->connections_end;
    LL_PREPEND(server->connections, server->connections_end, CnsConnection);

    server->connections_end->fd = client;
    server->connections_end->owner_server_index = i;
    server->connections_end->owner_client_index = (u32) -1;
    inet_ntop(AF_INET, &address.sin_addr,
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

static void main_loop_servers_receive_data(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->servers.len; ++i) {
    Server *server = ctx->servers.items + i;
    Data *data = &server->data;

    if (poll(server->connection_poll_fds.items,
             server->connection_poll_fds.len,
             server->receive_timeout) <= 0)
      continue;

    CnsConnection **connection = &server->connections;
    u32 i = 0;
    while (*connection && i < server->connection_poll_fds.len) {
      server->connection_poll_fds.items[i].events = POLLIN;

      if (server->connection_poll_fds.items[i].revents & POLLIN) {
        i32 len;
        while ((len = recv((*connection)->fd, data->items + data->len, data->cap - data->len, MSG_DONTWAIT)) > 0) {
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
}

static void main_loop_clients_confirm_connections(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->clients.len; ++i) {
    Client *client = ctx->clients.items + i;

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
            client_destroy(client);
            DA_REMOVE_AT(ctx->clients, i);
            --i;
          } else {
            client->connected = true;
          }
        }
      } else {
        client->disconnected_cb(ctx, &client->connection);
        DA_REMOVE_AT(ctx->clients, i);
        --i;
      }
    }
  }
}

static void main_loop_clients_receive_data(CnsCtx *ctx) {
  for (u32 i = 0; i < ctx->clients.len; ++i) {
    Client *client = ctx->clients.items + i;
    Data *data = &client->data;

    if (!client->connected)
      continue;

    struct pollfd poll_fd = { client->fd, POLLIN, 0 };

    if (poll(&poll_fd, 1, client->receive_timeout) <= 0)
      continue;

    if (poll_fd.revents & POLLIN) {
      i32 len;
      while ((len = recv(client->fd, data->items + data->len, data->cap - data->len, MSG_DONTWAIT)) > 0) {
        if ((u32) len == data->cap - data->len) {
          data->cap *= 2;
          data->items = realloc(data->items, data->cap);
        }
        data->len += len;
      }

      if (len == 0) {
        if (client->disconnected_cb)
          client->disconnected_cb(ctx, &client->connection);
        client_destroy(client);
        DA_REMOVE_AT(ctx->clients, i);
        --i;
        continue;
      }

      if (data->len > 0 && client->data_cb) {
        if (client->data_cb(ctx, &client->connection, data->items, data->len) != CnsResultOk) {
          client_destroy(client);
          DA_REMOVE_AT(ctx->clients, i);
          --i;
          continue;
        }
      }

      data->len = 0;
    }
  }
}

int cns_step(CnsCtx *ctx, unsigned int delay_ms) {
  if (ctx->stop || (ctx->servers.len == 0 && ctx->clients.len == 0))
    return 0;

  main_loop_servers_accept_connections(ctx);
  main_loop_servers_receive_data(ctx);

  if (ctx->clients_connected < ctx->clients.len)
    main_loop_clients_confirm_connections(ctx);
  main_loop_clients_receive_data(ctx);

  usleep(delay_ms);

  return !ctx->stop && (ctx->servers.len > 0 || ctx->clients.len > 0);
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

  for (u32 i = 0; i < ctx->clients.len; ++i)
    client_destroy(ctx->clients.items + i);
  if (ctx->clients.items)
    free(ctx->clients.items);

  free(ctx);
}

CnsError cns_listen(CnsCtx *ctx, unsigned short port, CnsListenInfo *info) {
  i32 enable = 1;

  Fd sock = socket(AF_INET, SOCK_STREAM, 0);
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

  if (listen(sock, 0) < 0) {
    close(sock);
    return CnsErrorCouldNotListen;
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

CnsError cns_connect(CnsCtx *ctx, char *addr, unsigned short port, CnsConnectInfo *info) {
  i32 enable = 1;

  Fd sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return CnsErrorCouldNotCreateSocket;

  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  fcntl(sock, F_SETFL, O_NONBLOCK);

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons(port);

  if (inet_pton(AF_INET, addr, &address.sin_addr) < 0) {
    close(sock);
    return CnsErrorInvalidAddress;
  }

  connect(sock, (struct sockaddr *) &address, sizeof(address));
  if (errno != EINPROGRESS) {
    close(sock);
    return CnsErrorCouldNotConnect;
  }

  CnsConnection connection = { sock, {}, NULL, (u32) -1, ctx->clients.len, NULL };
  u32 addr_len = strlen(addr);
  if (addr_len > sizeof(connection.address))
    addr_len = sizeof(connection.address);
  memcpy(connection.address, addr, addr_len);

  Data data;
  data.len = 0;
  data.cap = DEFAULT_DATA_BUFFER_CAP;
  data.items = malloc(data.cap);

  Client client = {
    sock,
    info->proto,
    info->receive_timeout,
    false,
    connection,
    data,
    info->connected_cb,
    info->data_cb,
    info->disconnected_cb,
  };
  DA_APPEND(ctx->clients, client);

  return CnsErrorOk;
}

void cns_send(CnsConnection *connection, unsigned char *data, unsigned long data_len) {
  send(connection->fd, data, data_len, MSG_DONTWAIT);
}

void cns_close(CnsCtx *ctx, CnsConnection *connection) {
  if (connection->owner_server_index != (u32) -1) {
    Server *server = ctx->servers.items + connection->owner_server_index;

    CnsConnection **_connection = &server->connections;
    u32 i = 0;
    while (_connection && i < server->connection_poll_fds.len) {
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
    client_destroy(ctx->clients.items + connection->owner_client_index);
    DA_REMOVE_AT(ctx->clients, connection->owner_client_index);
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
