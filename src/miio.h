#ifndef ROCKCTL_MIIO_H
#define ROCKCTL_MIIO_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char host[64];
  int port;              /* default 54321 */
  char token_path[160];  /* for lazy reopen after boot race / hang recovery */
  uint8_t token[16];
  int token_ok;          /* 1 if token bytes loaded */
  uint32_t device_id;
  uint32_t stamp;
  int sock;              /* -1 if not connected */
  int timeout_ms;
} miio_client;

/* token_path e.g. /mnt/data/miio/device.token (16 raw bytes or hex or ascii) */
int miio_open(miio_client *c, const char *host, int port, const char *token_path);
void miio_close(miio_client *c);
/* Prepare host/token without hello (for serve-when-miio-late). */
int miio_prepare(miio_client *c, const char *host, int port, const char *token_path);
/* Re-run hello if sock invalid; uses stored host/port/token_path. */
int miio_ensure(miio_client *c);

/* Call method; params_json may be "[]" or object; result_json malloc'd on success */
int miio_call(miio_client *c, const char *method, const char *params_json, char **result_json);

#endif
