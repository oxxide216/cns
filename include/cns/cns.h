#ifndef CNS_H
#define CNS_H

#include <stdio.h>

typedef enum {
  CnsErrorOk = 0,
  CnsErrorCouldNotCreateSocket,
  CnsErrorCouldNotBind,
  CnsErrorCouldNotListen,
  CnsErrorInvalidAddress,
  CnsErrorCouldNotConnect,
} CnsError;

typedef enum {
  CnsResultOk = 0,
  CnsResultNotOk,
} CnsResult;

typedef enum {
  CnsProtoTCP = 0,
} CnsProto;

typedef struct CnsCtx CnsCtx;

typedef struct CnsConnection CnsConnection;

// If any of these functions returns CnsResultNotOk, close the connection
typedef CnsResult (*CnsConnectedCallback)(CnsCtx *ctx, CnsConnection *connection);
typedef CnsResult (*CnsDataCallback)(CnsCtx *ctx, CnsConnection *connection, unsigned char *data, unsigned long data_len);
typedef void      (*CnsDisconnectedCallback)(CnsCtx *ctx, CnsConnection *connection);

typedef struct {
  CnsProto                proto;
  unsigned int            receive_timeout;
  CnsConnectedCallback    connected_cb;
  CnsDataCallback         data_cb;
  CnsDisconnectedCallback disconnected_cb;
} CnsListenInfo;

typedef struct {
  CnsProto                proto;
  unsigned int            receive_timeout;
  CnsConnectedCallback    connected_cb;
  CnsDataCallback         data_cb;
  CnsDisconnectedCallback disconnected_cb;
} CnsConnectInfo;

#ifdef __cplusplus
extern "C" {
#endif

// Context basics
CnsCtx *cns_create(void);
// stop = 0, continue = 1
int     cns_step(CnsCtx *ctx, unsigned int delay_ms);
void    cns_run(CnsCtx *ctx);
void    cns_stop(CnsCtx *ctx);
void    cns_destroy(CnsCtx *ctx);

// Networking
CnsError cns_listen(CnsCtx *ctx, unsigned short port, CnsListenInfo *info);
CnsError cns_connect(CnsCtx *ctx, char *addr, unsigned short port, CnsConnectInfo *info);
void     cns_send(CnsConnection *connection, unsigned char *data, unsigned long data_len);
void     cns_close(CnsCtx *ctx, CnsConnection *connection);

// Getters and setters
void *cns_get_user_data(CnsCtx *ctx);
void  cns_set_user_data(CnsCtx *ctx, void *user_data);
void *cns_get_connection_user_data(CnsConnection *connection);
void  cns_set_connection_user_data(CnsConnection *connection, void *user_data);
char *cns_get_connection_address(CnsConnection *connection);

// Utils
char *cns_get_error_str(CnsError error);
void  cns_make_non_blocking(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif // CNS_H
