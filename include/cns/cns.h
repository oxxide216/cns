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
  CnsProtoUDP,
} CnsProto;

typedef struct CnsCtx CnsCtx;
typedef struct CnsConnection CnsConnection;
typedef struct CnsUdpDest CnsUdpDest;
typedef struct CnsTimer CnsTimer;

// If any of these functions returns CnsResultNotOk, close the connection (or stop the timer)
typedef CnsResult (*CnsConnectedCallback)(CnsCtx *ctx, CnsConnection *connection);
typedef CnsResult (*CnsConnectionDataCallback)(CnsCtx *ctx, CnsConnection *connection, unsigned char *data, unsigned long data_len);
typedef CnsResult (*CnsDataCallback)(CnsCtx *ctx, unsigned char *data, unsigned long data_len);
typedef void      (*CnsDisconnectedCallback)(CnsCtx *ctx, CnsConnection *connection);
typedef CnsResult (*CnsTimerCallback)(CnsCtx *ctx, CnsTimer *timer);

typedef struct {
  CnsProto                  proto;
  unsigned int              receive_timeout;
  CnsConnectedCallback      connected_cb; // For UDP, called when packet from new
                                          // client is received for the first time,
                                          // before data_cb
  CnsConnectionDataCallback data_cb;
  CnsDisconnectedCallback   disconnected_cb; // TCP only
} CnsListenInfo;

typedef struct {
  unsigned int              receive_timeout;
  CnsConnectedCallback      connected_cb;
  CnsConnectionDataCallback data_cb;
  CnsDisconnectedCallback   disconnected_cb;
} CnsTcpConnectInfo;

typedef struct {
  unsigned int    receive_timeout;
  CnsDataCallback data_cb;
} CnsUdpInitInfo;

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

// Common networking (both TCP and UDP)
CnsError cns_listen(CnsCtx *ctx, unsigned short port, CnsListenInfo *info);
void     cns_close(CnsCtx *ctx, CnsConnection *connection);
// TCP
CnsError cns_tcp_connect(CnsCtx *ctx, char *addr, unsigned short port, CnsTcpConnectInfo *info);
void     cns_tcp_send(CnsConnection *connection, unsigned char *data, unsigned long data_len);
// UDP
CnsError    cns_udp_init(CnsCtx *ctx, CnsUdpInitInfo *info);
void        cns_udp_enable_broadcast_send(CnsCtx *ctx);
void        cns_udp_enable_multicast_receive(CnsCtx *ctx, char *group);
CnsUdpDest *cns_udp_create_dest(CnsCtx *ctx, char *addr, unsigned short port);
void        cns_udp_send(CnsUdpDest *dest, unsigned char *data, unsigned long data_len);
void        cns_udp_destroy_dest(CnsUdpDest *dest);

// Timers
// repeat_timeout_ms == 0 means no repeating
CnsTimer *cns_start_timer(CnsCtx *ctx, unsigned long start_timeout_ms,
                          unsigned long repeat_timeout_ms, CnsTimerCallback tick_cb);
void      cns_stop_timer(CnsCtx *ctx, CnsTimer *timer);

// Common getters and setters
void           *cns_get_user_data(CnsCtx *ctx);
void            cns_set_user_data(CnsCtx *ctx, void *user_data);
void           *cns_get_connection_user_data(CnsConnection *connection);
void            cns_set_connection_user_data(CnsConnection *connection, void *user_data);
char           *cns_get_connection_address(CnsConnection *connection);
unsigned short  cns_get_connection_port(CnsConnection *connection);
void           *cns_get_timer_user_data(CnsTimer *timer);
void            cns_set_timer_user_data(CnsTimer *timer, void *user_data);
// UDP-related getters
CnsUdpDest     *cns_udp_get_connection_dest(CnsConnection *connection);
char           *cns_udp_get_dest_address(CnsUdpDest *dest);
unsigned short  cns_udp_get_dest_port(CnsUdpDest *dest);

// Utils
char *cns_get_error_str(CnsError error);

#ifdef __cplusplus
}
#endif

#endif // CNS_H
