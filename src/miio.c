#include "miio.h"
#include "../third_party/aes.h"
#include "../third_party/md5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

static int load_token(const char *path, uint8_t token[16]) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  uint8_t buf[64];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  if (n == 16) { memcpy(token, buf, 16); return 0; }
  if (n == 32) {
    /* hex */
    for (int i = 0; i < 16; i++) {
      unsigned v;
      if (sscanf((char *)buf + i * 2, "%2x", &v) != 1) return -1;
      token[i] = (uint8_t)v;
    }
    return 0;
  }
  if (n >= 16) { memcpy(token, buf, 16); return 0; }
  return -1;
}

static void md5_token_key_iv(const uint8_t token[16], uint8_t key[16], uint8_t iv[16]) {
  md5(token, 16, key);
  uint8_t tmp[32];
  memcpy(tmp, key, 16);
  memcpy(tmp + 16, token, 16);
  md5(tmp, 32, iv);
}

static uint8_t *aes_encrypt(const uint8_t token[16], const uint8_t *plain, size_t len, size_t *out_len) {
  uint8_t key[16], iv[16];
  md5_token_key_iv(token, key, iv);
  size_t pad = 16 - (len % 16);
  size_t n = len + pad;
  uint8_t *buf = malloc(n);
  if (!buf) return NULL;
  memcpy(buf, plain, len);
  memset(buf + len, (int)pad, pad);
  struct AES_ctx ctx;
  AES_init_ctx_iv(&ctx, key, iv);
  AES_CBC_encrypt_buffer(&ctx, buf, n);
  *out_len = n;
  return buf;
}

static uint8_t *aes_decrypt(const uint8_t token[16], const uint8_t *enc, size_t len, size_t *out_len) {
  if (len == 0 || (len % 16) != 0) return NULL;
  uint8_t key[16], iv[16];
  md5_token_key_iv(token, key, iv);
  uint8_t *buf = malloc(len);
  if (!buf) return NULL;
  memcpy(buf, enc, len);
  struct AES_ctx ctx;
  AES_init_ctx_iv(&ctx, key, iv);
  AES_CBC_decrypt_buffer(&ctx, buf, len);
  uint8_t pad = buf[len - 1];
  if (pad == 0 || pad > 16) { free(buf); return NULL; }
  *out_len = len - pad;
  buf[*out_len] = 0;
  return buf;
}

int miio_prepare(miio_client *c, const char *host, int port, const char *token_path) {
  if (!c) return -1;
  memset(c, 0, sizeof *c);
  c->sock = -1;
  c->timeout_ms = 1500; /* lean: fail fast — Dash must not wait multi-second miio */
  snprintf(c->host, sizeof c->host, "%s", host ? host : "127.0.0.1");
  c->port = port > 0 ? port : 54321;
  if (token_path && token_path[0])
    snprintf(c->token_path, sizeof c->token_path, "%s", token_path);
  else
    snprintf(c->token_path, sizeof c->token_path, "%s", "/mnt/data/miio/device.token");
  if (load_token(c->token_path, c->token) != 0) {
    c->token_ok = 0;
    return -1;
  }
  c->token_ok = 1;
  return 0;
}

int miio_open(miio_client *c, const char *host, int port, const char *token_path) {
  if (miio_prepare(c, host, port, token_path) != 0) return -1;
  return miio_ensure(c);
}

int miio_ensure(miio_client *c) {
  if (!c) return -1;
  if (c->sock >= 0) return 0;
  if (!c->token_ok) {
    if (!c->token_path[0]) return -1;
    if (load_token(c->token_path, c->token) != 0) return -1;
    c->token_ok = 1;
  }
  c->sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (c->sock < 0) return -1;
  if (c->timeout_ms <= 0) c->timeout_ms = 3000;
  struct timeval tv = { .tv_sec = c->timeout_ms / 1000, .tv_usec = (c->timeout_ms % 1000) * 1000 };
  setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  /* handshake hello — miio stack may not be up yet at boot */
  uint8_t hello[32];
  memset(hello, 0xff, 32);
  hello[0] = 0x21; hello[1] = 0x31;
  hello[2] = 0x00; hello[3] = 0x20;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)c->port);
  inet_pton(AF_INET, c->host, &addr.sin_addr);
  if (sendto(c->sock, hello, 32, 0, (struct sockaddr *)&addr, sizeof addr) < 0) {
    close(c->sock); c->sock = -1; return -1;
  }
  uint8_t resp[64];
  socklen_t al = sizeof addr;
  ssize_t n = recvfrom(c->sock, resp, sizeof resp, 0, (struct sockaddr *)&addr, &al);
  if (n < 32) {
    close(c->sock); c->sock = -1; return -1;
  }
  c->device_id = ((uint32_t)resp[8] << 24) | ((uint32_t)resp[9] << 16) |
                ((uint32_t)resp[10] << 8) | (uint32_t)resp[11];
  c->stamp = ((uint32_t)resp[12] << 24) | ((uint32_t)resp[13] << 16) |
             ((uint32_t)resp[14] << 8) | (uint32_t)resp[15];
  return 0;
}

void miio_close(miio_client *c) {
  if (!c) return;
  if (c->sock >= 0) close(c->sock);
  c->sock = -1;
}

/* Refresh device_id/stamp (long-lived serve process goes stale otherwise). */
static int miio_hello(miio_client *c) {
  if (c->sock < 0) return -1;
  uint8_t hello[32];
  memset(hello, 0xff, 32);
  hello[0] = 0x21; hello[1] = 0x31;
  hello[2] = 0x00; hello[3] = 0x20;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)c->port);
  inet_pton(AF_INET, c->host, &addr.sin_addr);
  /* Drain stale datagrams without blocking.
   * NOTE: SO_RCVTIMEO={0,0} means *infinite* wait on Linux, not non-blocking.
   * That previously hung rockctl serve forever (dash dead on :8080). */
  {
    uint8_t junk[64];
    while (recvfrom(c->sock, junk, sizeof junk, MSG_DONTWAIT, NULL, NULL) > 0) {}
    struct timeval tv = { .tv_sec = c->timeout_ms / 1000,
                          .tv_usec = (c->timeout_ms % 1000) * 1000 };
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  }
  if (sendto(c->sock, hello, 32, 0, (struct sockaddr *)&addr, sizeof addr) < 0) return -1;
  uint8_t resp[64];
  socklen_t al = sizeof addr;
  ssize_t n = recvfrom(c->sock, resp, sizeof resp, 0, (struct sockaddr *)&addr, &al);
  if (n < 32) return -1;
  c->device_id = ((uint32_t)resp[8] << 24) | ((uint32_t)resp[9] << 16) |
                ((uint32_t)resp[10] << 8) | (uint32_t)resp[11];
  c->stamp = ((uint32_t)resp[12] << 24) | ((uint32_t)resp[13] << 16) |
             ((uint32_t)resp[14] << 8) | (uint32_t)resp[15];
  return 0;
}

static int miio_call_once(miio_client *c, const char *method, const char *params_json, char **result_json) {
  if (result_json) *result_json = NULL;
  if (!params_json) params_json = "[]";
  c->stamp++;
  char payload[1024];
  int plen = snprintf(payload, sizeof payload,
    "{\"id\":%u,\"method\":\"%s\",\"params\":%s}",
    c->stamp, method, params_json);
  if (plen < 0 || plen >= (int)sizeof payload) return -1;

  size_t elen = 0;
  uint8_t *enc = aes_encrypt(c->token, (const uint8_t *)payload, (size_t)plen, &elen);
  if (!enc) return -1;

  uint16_t length = (uint16_t)(32 + elen);
  uint8_t header[32];
  memset(header, 0, 32);
  header[0] = 0x21; header[1] = 0x31;
  header[2] = (uint8_t)(length >> 8); header[3] = (uint8_t)(length & 0xff);
  header[8] = (uint8_t)(c->device_id >> 24);
  header[9] = (uint8_t)(c->device_id >> 16);
  header[10] = (uint8_t)(c->device_id >> 8);
  header[11] = (uint8_t)(c->device_id);
  header[12] = (uint8_t)(c->stamp >> 24);
  header[13] = (uint8_t)(c->stamp >> 16);
  header[14] = (uint8_t)(c->stamp >> 8);
  header[15] = (uint8_t)(c->stamp);

  uint8_t chk_in[16 + 16 + 4096];
  memcpy(chk_in, header, 16);
  memcpy(chk_in + 16, c->token, 16);
  memcpy(chk_in + 32, enc, elen);
  md5(chk_in, 32 + elen, header + 16);

  uint8_t *pkt = malloc(length);
  if (!pkt) { free(enc); return -1; }
  memcpy(pkt, header, 32);
  memcpy(pkt + 32, enc, elen);
  free(enc);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)c->port);
  inet_pton(AF_INET, c->host, &addr.sin_addr);
  if (sendto(c->sock, pkt, length, 0, (struct sockaddr *)&addr, sizeof addr) < 0) {
    free(pkt); return -1;
  }
  free(pkt);

  uint8_t resp[8192];
  socklen_t al = sizeof addr;
  ssize_t rn = recvfrom(c->sock, resp, sizeof resp, 0, (struct sockaddr *)&addr, &al);
  if (rn < 32) return -1;
  size_t dlen = 0;
  uint8_t *plain = aes_decrypt(c->token, resp + 32, (size_t)rn - 32, &dlen);
  if (!plain) return -1;
  plain[dlen] = 0;
  if (result_json) *result_json = (char *)plain;
  else free(plain);
  return 0;
}

int miio_call(miio_client *c, const char *method, const char *params_json, char **result_json) {
  if (!c) return -1;
  /* Lazy connect: serve can start before firmware miio answers hello. */
  if (c->sock < 0) {
    if (miio_ensure(c) != 0) return -1;
  }
  if (miio_call_once(c, method, params_json, result_json) == 0) return 0;
  /* Long-running rockctl serve: stamp/socket go stale — re-hello and retry once. */
  if (miio_hello(c) != 0) {
    miio_close(c);
    if (miio_ensure(c) != 0) return -1;
  }
  if (result_json && *result_json) { free(*result_json); *result_json = NULL; }
  return miio_call_once(c, method, params_json, result_json);
}
