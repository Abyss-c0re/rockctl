#include "drive_path.h"
#include "http.h"
#include "clanker_dash_html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

/* Minimal openapi yaml embedded */
static const char OPENAPI_YAML[] =
"openapi: 3.0.3\n"
"info:\n"
"  title: rockctl API\n"
"  version: 0.1.0\n"
"  description: Local-first Roborock control (miio). Replaces Valetudo cloud UI with CLI/HTTP.\n"
"servers:\n"
"  - url: http://{host}:{port}\n"
"    variables:\n"
"      host: { default: 127.0.0.1 }\n"
"      port: { default: '8080' }\n"
"paths:\n"
"  /api/v1/health:\n"
"    get:\n"
"      summary: Liveness\n"
"      responses: { '200': { description: OK } }\n"
"  /api/v1/status:\n"
"    get:\n"
"      summary: Robot status (get_status)\n"
"      responses: { '200': { description: miio result JSON } }\n"
"  /api/v1/control:\n"
"    post:\n"
"      summary: Basic control\n"
"      requestBody:\n"
"        required: true\n"
"        content:\n"
"          application/json:\n"
"            schema:\n"
"              type: object\n"
"              required: [action]\n"
"              properties:\n"
"                action:\n"
"                  type: string\n"
"                  enum: [start, stop, pause, home, spot, locate]\n"
"      responses: { '200': { description: miio result } }\n"
"  /api/v1/fan:\n"
"    put:\n"
"      summary: Fan speed preset\n"
"      requestBody:\n"
"        content:\n"
"          application/json:\n"
"            schema:\n"
"              type: object\n"
"              properties:\n"
"                level: { type: string, enum: [quiet, balanced, turbo, max] }\n"
"      responses: { '200': { description: OK } }\n"
"  /api/v1/raw:\n"
"    post:\n"
"      summary: Raw miio method\n"
"      requestBody:\n"
"        content:\n"
"          application/json:\n"
"            schema:\n"
"              type: object\n"
"              required: [method]\n"
"              properties:\n"
"                method: { type: string }\n"
"                params: { description: JSON array/object as string or value }\n"
"      responses: { '200': { description: raw result } }\n"
"  /openapi.yaml:\n"
"    get:\n"
"      summary: OpenAPI document\n"
"      responses: { '200': { description: YAML } }\n";

static void send_all(int fd, const char *b, size_t n) {
  while (n) {
    ssize_t w = write(fd, b, n);
    if (w < 0) { if (errno == EINTR) continue; return; }
    b += w; n -= (size_t)w;
  }
}
static void resp(int fd, int code, const char *ctype, const char *body) {
  size_t bl = body ? strlen(body) : 0;
  char h[512];
  const char *reason =
    code==200?"OK":code==204?"No Content":code==400?"Bad Request":
    code==401?"Unauthorized":code==404?"Not Found":"Error";
  int n = snprintf(h, sizeof h,
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
    "Connection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type, Authorization, X-Nanobot-Peer-Token, X-Labauth-Master\r\n"
    "Access-Control-Max-Age: 86400\r\n"
    "\r\n",
    code, reason, ctype, bl);
  send_all(fd, h, (size_t)n);
  if (body && bl) send_all(fd, body, bl);
}

/* OPTIONS preflight (e.g. :8787 UI → :8080 API). */
static void resp_cors_preflight(int fd) {
  const char *h =
    "HTTP/1.1 204 No Content\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type, Authorization, X-Nanobot-Peer-Token, X-Labauth-Master\r\n"
    "Access-Control-Max-Age: 86400\r\n"
    "\r\n";
  send_all(fd, h, strlen(h));
}

static void resp_bin(int fd, int code, const char *ctype, const char *disp,
                     const void *body, size_t bl) {
  char h[384];
  int n = snprintf(h, sizeof h,
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
    "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n"
    "%s%s%s"
    "\r\n",
    code, code==200?"OK":"Error", ctype, bl,
    disp && disp[0] ? "Content-Disposition: attachment; filename=\"" : "",
    disp && disp[0] ? disp : "",
    disp && disp[0] ? "\"\r\n" : "");
  send_all(fd, h, (size_t)n);
  if (body && bl) send_all(fd, (const char *)body, bl);
}

static int read_file_bin(const char *path, char **out, size_t *out_len) {
  *out = NULL; if (out_len) *out_len = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long n = ftell(f);
  if (n < 0 || n > 8*1024*1024) { fclose(f); return -1; }
  rewind(f);
  char *b = malloc((size_t)n + 1);
  if (!b) { fclose(f); return -1; }
  size_t rd = fread(b, 1, (size_t)n, f);
  fclose(f);
  b[rd] = 0;
  *out = b;
  if (out_len) *out_len = rd;
  return 0;
}

static void parse_xy_file(const char *path, int *x, int *y, int *ok) {
  *ok = 0; *x = *y = 0;
  char *b = NULL; size_t n = 0;
  if (read_file_bin(path, &b, &n) != 0 || !b) return;
  /* text form: x = 6540;\ny = -2376;\nangle = ... */
  const char *px = strstr(b, "x");
  const char *py = strstr(b, "y");
  if (px) {
    while (*px && *px != '=' && *px != ':') px++;
    if (*px) { px++; *x = atoi(px); *ok = 1; }
  }
  if (py) {
    while (*py && *py != '=' && *py != ':') py++;
    if (*py) { py++; *y = atoi(py); *ok = 1; }
  }
  if (!*ok) {
    const char *jx = strstr(b, "\"x\"");
    const char *jy = strstr(b, "\"y\"");
    if (jx) { const char *c = strchr(jx, ':'); if (c) { *x = atoi(c+1); *ok = 1; } }
    if (jy) { const char *c = strchr(jy, ':'); if (c) { *y = atoi(c+1); *ok = 1; } }
  }
  free(b);
}

/* RRSLAM last_map:
 *  magic "RRSLAM" (6)
 *  top  @0x0e i32  (map cell of image top edge)
 *  left @0x12 i32  (map cell of image left edge)
 *  w    @0x16 i32  h @0x1a i32
 *  pixels @46, resolution 50 mm/cell
 *  SLAM mm: x = (left + px) * 50,  y = (top + py) * 50
 */
static int rrslam_dims(const char *bin, size_t n, int *w, int *h, int *off,
                       int *top, int *left) {
  *w = *h = 0; *off = 0;
  if (top) *top = 0;
  if (left) *left = 0;
  if (n < 50 || memcmp(bin, "RRSLAM", 6) != 0) return -1;
  if (n >= 0x1e) {
    if (top) memcpy(top, bin + 0x0e, 4);
    if (left) memcpy(left, bin + 0x12, 4);
    memcpy(w, bin + 0x16, 4);
    memcpy(h, bin + 0x1a, 4);
  }
  if (*w < 32 || *w > 4096 || *h < 32 || *h > 4096) return -1;
  *off = 46;
  if ((size_t)(*off + (*w) * (*h)) > n) {
    size_t pix = (size_t)(*w) * (size_t)(*h);
    if (n > pix) *off = (int)(n - pix);
    else return -1;
  }
  return 0;
}


/* 3D export offloaded to BlackCube/Groot browser or map_3d_worker.py */

static int music_mode_path(char *out, size_t n) {
  snprintf(out, n, "/mnt/data/rockctl/music_mode");
  return 0;
}

static char *curl_get(const char *url) {
  /* tiny helper not needed — music uses mode file + shell */
  (void)url; return NULL;
}

static int run_sh(const char *cmd) {
  pid_t p = fork();
  if (p < 0) return -1;
  if (p == 0) {
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  int st = 0;
  waitpid(p, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Max music upload: 3 MB WAV (robot disk is tight). */
#define ROCK_HTTP_MAX_BODY (3 * 1024 * 1024)

static char *read_req(int fd, size_t *out_len) {
  size_t cap = 8192, len = 0;
  char *b = malloc(cap);
  if (out_len) *out_len = 0;
  if (!b) return NULL;
  /* Bound stuck peers so one thread cannot pin forever */
  {
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  /* read headers first */
  while (1) {
    if (len + 1024 > cap) {
      cap *= 2;
      if (cap > 256 * 1024) { free(b); return NULL; }
      char *n = realloc(b, cap);
      if (!n) { free(b); return NULL; }
      b = n;
    }
    ssize_t r = read(fd, b + len, cap - len - 1);
    if (r <= 0) break;
    len += (size_t)r;
    b[len] = 0;
    if (strstr(b, "\r\n\r\n")) break;
  }
  char *hdrend = strstr(b, "\r\n\r\n");
  if (!hdrend) { if (out_len) *out_len = len; return b; }
  size_t hlen = (size_t)(hdrend + 4 - b);
  size_t cl = 0;
  int have_cl = 0;
  const char *clh = strstr(b, "Content-Length:");
  if (!clh) clh = strstr(b, "content-length:");
  if (clh) { cl = (size_t)strtoul(clh + 15, NULL, 10); have_cl = 1; }
  if (have_cl && cl > ROCK_HTTP_MAX_BODY) { free(b); return NULL; }
  /* GET/HEAD/OPTIONS or empty body: stop at headers (keep-alive safe) */
  int no_body = (strncmp(b, "GET ", 4) == 0 || strncmp(b, "HEAD ", 5) == 0 ||
                 strncmp(b, "OPTIONS ", 8) == 0);
  if (no_body || !have_cl || cl == 0) {
    if (out_len) *out_len = len;
    return b;
  }
  size_t need = hlen + cl;
  if (need + 1 > cap) {
    char *n = realloc(b, need + 1);
    if (!n) { free(b); return NULL; }
    b = n; cap = need + 1;
  }
  while (len < need) {
    ssize_t r = read(fd, b + len, need - len);
    if (r <= 0) break;
    len += (size_t)r;
  }
  b[len] = 0;
  if (out_len) *out_len = len;
  return b;
}

static void write_small(const char *path, const char *s) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputs(s, f);
  if (s[0] && s[strlen(s)-1] != '\n') fputc('\n', f);
  fclose(f);
}

static void read_small(const char *path, char *out, size_t n, const char *def) {
  out[0] = 0;
  FILE *f = fopen(path, "r");
  if (!f) { snprintf(out, n, "%s", def ? def : ""); return; }
  if (!fgets(out, (int)n, f)) { fclose(f); snprintf(out, n, "%s", def ? def : ""); return; }
  fclose(f);
  size_t L = strlen(out);
  while (L && (out[L-1]=='\n'||out[L-1]=='\r'||out[L-1]==' ')) out[--L]=0;
}

static int safe_track_name(const char *name, char *out, size_t n) {
  if (!name || !name[0] || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return -1;
  size_t L = strlen(name);
  if (L < 5 || L > 80) return -1;
  /* require .wav */
  const char *dot = strrchr(name, '.');
  if (!dot || (strcasecmp(dot, ".wav") != 0)) return -1;
  for (const char *p = name; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (!(isalnum(c) || c=='_' || c=='-' || c=='.')) return -1;
  }
  snprintf(out, n, "%s", name);
  return 0;
}

/* Event sound library: /mnt/data/rockctl/sounds + per-event config under sounds/events/<id>/ */
#define SOUNDS_DIR "/mnt/data/rockctl/sounds"
#define SOUNDS_EVT_DIR "/mnt/data/rockctl/sounds/events"
#define SOUNDS_MAX_UPLOAD (512 * 1024) /* 512 KB short clips */

static int safe_event_id(const char *name, char *out, size_t n) {
  if (!name || !name[0] || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\'))
    return -1;
  size_t L = strlen(name);
  if (L < 1 || L > 32) return -1;
  for (const char *p = name; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (!(isalnum(c) || c == '_' || c == '-')) return -1;
  }
  snprintf(out, n, "%s", name);
  return 0;
}

static void sounds_ensure_dirs(void) {
  mkdir("/mnt/data/rockctl", 0755);
  mkdir(SOUNDS_DIR, 0755);
  mkdir(SOUNDS_EVT_DIR, 0755);
}

static void sounds_evt_path(char *out, size_t n, const char *evt, const char *leaf) {
  snprintf(out, n, "%s/%s/%s", SOUNDS_EVT_DIR, evt, leaf);
}

static void sounds_read_evt(const char *evt, const char *leaf, char *out, size_t n, const char *def) {
  char p[256];
  sounds_evt_path(p, sizeof p, evt, leaf);
  read_small(p, out, n, def);
}

static void sounds_write_evt(const char *evt, const char *leaf, const char *val) {
  char dir[256], p[288];
  snprintf(dir, sizeof dir, "%s/%s", SOUNDS_EVT_DIR, evt);
  mkdir(dir, 0755);
  sounds_evt_path(p, sizeof p, evt, leaf);
  write_small(p, val);
}

/* Known event catalog (id, human label). Wired: ouch. Others are config-only until a watcher uses them. */
static const struct { const char *id; const char *label; const char *def_file; } SOUND_EVENTS[] = {
  { "ouch", "Obstacle / bumper hit", "ouch.wav" },
  { "dock", "Docked / returned home", "" },
  { "start_clean", "Start cleaning", "" },
  { "done_clean", "Cleaning finished", "" },
  { "low_battery", "Low battery", "" },
  { NULL, NULL, NULL }
};

static void sounds_sync_ouch_enabled(const char *en) {
  /* Keep legacy ouch_enabled file in sync for older tooling */
  write_small("/mnt/data/rockctl/ouch_enabled", en && en[0] ? en : "1");
}

static void ensure_music_watchdog(void) {
  run_sh("mkdir -p /mnt/data/rockctl/music; "
         "if [ -x /mnt/data/rockctl/bin/clean_music_loop.sh ]; then W=/mnt/data/rockctl/bin/clean_music_loop.sh; "
         "elif [ -x /mnt/data/clean_music_loop.sh ]; then W=/mnt/data/clean_music_loop.sh; "
         "else W=; fi; "
         "if [ -n \"$W\" ] && ! kill -0 $(cat /mnt/data/clean_music.pid 2>/dev/null) 2>/dev/null; then "
         "$W >>/mnt/data/clean_music.log 2>&1 & fi");
}

static const char *json_str(const char *j, const char *key, char *out, size_t outsz) {
  char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",key);
  const char *p=strstr(j,pat); if(!p) return NULL;
  p=strchr(p+strlen(pat),':'); if(!p) return NULL; p++;
  while(*p==' '||*p=='\t') p++;
  if(*p!='"') return NULL; p++;
  size_t i=0;
  while(*p && *p!='"' && i+1<outsz){
    if(*p=='\\'&&p[1]){p++;}
    out[i++]=*p++;
  }
  out[i]=0; return out;
}

/* Parse JSON number (or quoted digit string) for key; returns 1 if found. */
static int json_int(const char *j, const char *key, int *out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(j, pat);
  if (!p) return 0;
  p = strchr(p + strlen(pat), ':');
  if (!p) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '"') p++;
  if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
  *out = atoi(p);
  return 1;
}

static int do_action(miio_client *m, const char *action, char **out) {
  const char *method = NULL;
  if (!strcmp(action,"start")) method="app_start";
  else if (!strcmp(action,"stop")) method="app_stop";
  else if (!strcmp(action,"pause")) method="app_pause";
  else if (!strcmp(action,"home")||!strcmp(action,"dock")||!strcmp(action,"charge")) {
    /* Home often no-ops while app_rc_* / manual is engaged. Drop RC first,
     * pause any active clean, then app_charge. Failures on pre-steps are OK. */
    char *tmp = NULL;
    miio_call(m, "app_rc_end", "[]", &tmp);
    free(tmp);
    tmp = NULL;
    miio_call(m, "app_pause", "[]", &tmp);
    free(tmp);
    return miio_call(m, "app_charge", "[]", out);
  }
  else if (!strcmp(action,"spot")) method="app_spot";
  else if (!strcmp(action,"locate")||!strcmp(action,"find")) method="find_me";
  else if (!strcmp(action,"rc_start")||!strcmp(action,"manual_start")||!strcmp(action,"manual"))
    method="app_rc_start";
  else if (!strcmp(action,"rc_end")||!strcmp(action,"manual_end")||!strcmp(action,"manual_stop"))
    method="app_rc_end";
  else return -2;
  return miio_call(m, method, "[]", out);
}

/* Parse JSON number (int or float) for key; returns 1 if found. */
static int json_double(const char *j, const char *key, double *out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(j, pat);
  if (!p) return 0;
  p = strchr(p + strlen(pat), ':');
  if (!p) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '"') p++;
  if (!(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9'))) return 0;
  *out = atof(p);
  return 1;
}

/* Manual drive (app_rc_*): no cleaning. LDS/nav for remote control only. */
static int do_manual(miio_client *m, const char *body, char **out) {
  char action[32] = {0};
  json_str(body, "action", action, sizeof action);
  if (!action[0]) json_str(body, "cmd", action, sizeof action);
  if (!action[0]) return -2;

  if (!strcmp(action, "start") || !strcmp(action, "engage") ||
      !strcmp(action, "rc_start") || !strcmp(action, "on")) {
    return miio_call(m, "app_rc_start", "[]", out);
  }
  if (!strcmp(action, "stop") || !strcmp(action, "end") || !strcmp(action, "disengage") ||
      !strcmp(action, "rc_end") || !strcmp(action, "off")) {
    return miio_call(m, "app_rc_end", "[]", out);
  }

  /* Directives: move | forward|back|left|right|halt|brake */
  double velocity = 0, omega = 0;
  int duration = 400, seqnum = 1;
  json_double(body, "velocity", &velocity);
  json_double(body, "omega", &omega);
  json_int(body, "duration", &duration);
  json_int(body, "seqnum", &seqnum);
  if (duration < 50) duration = 50;
  if (duration > 5000) duration = 5000;
  if (seqnum < 1) seqnum = 1;

  /* speed preset for directional pads: quiet|slow|med|fast|max */
  char speed[16] = {0};
  json_str(body, "speed", speed, sizeof speed);
  double vmag = 0.18, omag = 1.0;
  if (!strcmp(speed, "quiet") || !strcmp(speed, "slow") || !strcmp(speed, "low")) {
    vmag = 0.12; omag = 0.7;
  } else if (!strcmp(speed, "med") || !strcmp(speed, "medium") || !strcmp(speed, "normal") ||
             !strcmp(speed, "balanced")) {
    vmag = 0.20; omag = 1.1;
  } else if (!strcmp(speed, "fast") || !strcmp(speed, "turbo")) {
    vmag = 0.28; omag = 1.6;
  } else if (!strcmp(speed, "max")) {
    vmag = 0.32; omag = 2.0;
  }

  if (!strcmp(action, "forward") || !strcmp(action, "fwd") || !strcmp(action, "up")) {
    velocity = vmag; omega = 0;
  } else if (!strcmp(action, "back") || !strcmp(action, "backward") || !strcmp(action, "down") ||
             !strcmp(action, "rev")) {
    velocity = -vmag; omega = 0;
  } else if (!strcmp(action, "left") || !strcmp(action, "ccw")) {
    velocity = 0; omega = omag;
  } else if (!strcmp(action, "right") || !strcmp(action, "cw")) {
    velocity = 0; omega = -omag;
  } else if (!strcmp(action, "halt") || !strcmp(action, "brake") || !strcmp(action, "idle")) {
    velocity = 0; omega = 0;
  } else if (!strcmp(action, "move")) {
    /* velocity/omega already from body */
    if (velocity > 0.35) velocity = 0.35;
    if (velocity < -0.35) velocity = -0.35;
    if (omega > 3.1) omega = 3.1;
    if (omega < -3.1) omega = -3.1;
  } else {
    return -2;
  }

  char params[160];
  snprintf(params, sizeof params,
           "[{\"velocity\":%.4f,\"omega\":%.4f,\"duration\":%d,\"seqnum\":%d}]",
           velocity, omega, duration, seqnum);
  return miio_call(m, "app_rc_move", params, out);
}

static int fan_mode(const char *level) {
  /* Roborock custom modes commonly: 101 quiet, 102 balanced, 103 turbo, 104 max */
  if (!strcmp(level,"quiet")||!strcmp(level,"silent")||!strcmp(level,"min")) return 101;
  if (!strcmp(level,"balanced")||!strcmp(level,"normal")||!strcmp(level,"medium")||!strcmp(level,"med")) return 102;
  if (!strcmp(level,"turbo")||!strcmp(level,"strong")) return 103;
  if (!strcmp(level,"max")||!strcmp(level,"max+")) return 104;
  return -1;
}

/* Mop water box custom modes (common Roborock): 200 off, 201 low, 202 med, 203 high */
static int water_mode(const char *level) {
  if (!strcmp(level,"off")||!strcmp(level,"none")||!strcmp(level,"dry")) return 200;
  if (!strcmp(level,"low")||!strcmp(level,"min")||!strcmp(level,"quiet")) return 201;
  if (!strcmp(level,"medium")||!strcmp(level,"med")||!strcmp(level,"normal")||!strcmp(level,"balanced")) return 202;
  if (!strcmp(level,"high")||!strcmp(level,"max")||!strcmp(level,"wet")) return 203;
  return -1;
}

static int proc_alive(const char *pidpath) {
  FILE *np = fopen(pidpath, "r");
  int pid = 0, ok = 0;
  if (np && fscanf(np, "%d", &pid) == 1 && pid > 1) {
    char sp[64];
    snprintf(sp, sizeof sp, "/proc/%d", pid);
    ok = (access(sp, F_OK) == 0);
  }
  if (np) fclose(np);
  return ok;
}

static int shell_enabled_flag(void) {
  char b[16];
  read_small("/mnt/data/nanobot/shell_enabled", b, sizeof b, "1");
  if (!strcmp(b, "0") || !strcasecmp(b, "off") || !strcasecmp(b, "false") || !strcasecmp(b, "disabled"))
    return 0;
  return 1;
}

static int watcher_enabled_flag(void) {
  char b[16];
  read_small("/mnt/data/nanobot/watcher_enabled", b, sizeof b, "0");
  if (!strcmp(b, "1") || !strcasecmp(b, "on") || !strcasecmp(b, "true") || !strcasecmp(b, "enabled"))
    return 1;
  return 0;
}

/* KEY=val lines in /mnt/data/nanobot/settings (same file run.sh reads). */
static void nanobot_settings_get(const char *key, char *out, size_t n, const char *def) {
  if (out && n) out[0] = 0;
  if (!key || !out || n < 2) return;
  FILE *f = fopen("/mnt/data/nanobot/settings", "r");
  if (!f) {
    if (def) snprintf(out, n, "%s", def);
    return;
  }
  char line[256];
  size_t kl = strlen(key);
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || !*p) continue;
    if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
      char *v = p + kl + 1;
      size_t L = strlen(v);
      while (L && (v[L - 1] == '\n' || v[L - 1] == '\r' || v[L - 1] == ' ')) v[--L] = 0;
      snprintf(out, n, "%s", v);
      fclose(f);
      return;
    }
  }
  fclose(f);
  if (def) snprintf(out, n, "%s", def);
}

static int nanobot_settings_set(const char *key, const char *val) {
  if (!key || !val) return -1;
  char path[] = "/mnt/data/nanobot/settings";
  char tmp[] = "/mnt/data/nanobot/settings.tmp";
  mkdir("/mnt/data/nanobot", 0755);
  FILE *in = fopen(path, "r");
  FILE *out = fopen(tmp, "w");
  if (!out) {
    if (in) fclose(in);
    return -1;
  }
  int found = 0;
  size_t kl = strlen(key);
  if (in) {
    char line[256];
    while (fgets(line, sizeof line, in)) {
      char *p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (!found && strncmp(p, key, kl) == 0 && p[kl] == '=') {
        fprintf(out, "%s=%s\n", key, val);
        found = 1;
      } else {
        fputs(line, out);
      }
    }
    fclose(in);
  } else {
    fputs("# Survives reboot — boot via run.sh\n", out);
  }
  if (!found) fprintf(out, "%s=%s\n", key, val);
  fclose(out);
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static int ui_enabled_flag(void) {
  char b[32];
  nanobot_settings_get("UI", b, sizeof b, "off");
  if (!strcasecmp(b, "on") || !strcmp(b, "1") || !strcasecmp(b, "true")) return 1;
  /* live process may still be serving www even if settings lag */
  return 0;
}

/* Best-effort STA link: ip on wlan0 */
static int wifi_sta_up(char *ip_out, size_t n) {
  if (ip_out && n) ip_out[0] = 0;
  FILE *fp = popen("ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}' | head -1", "r");
  if (!fp) return 0;
  char line[64] = {0};
  if (fgets(line, sizeof line, fp)) {
    size_t L = strlen(line);
    while (L && (line[L-1]=='\n'||line[L-1]=='\r'||line[L-1]==' ')) line[--L]=0;
    char *slash = strchr(line, '/');
    if (slash) *slash = 0;
    if (ip_out && n && line[0]) snprintf(ip_out, n, "%s", line);
  }
  pclose(fp);
  return ip_out && ip_out[0] ? 1 : 0;
}

/* Apply fan + water then start clean N times (miio). cycles clamped 1..3 */
static int apply_clean_settings(miio_client *m, const char *fan, const char *water, int cycles, const char *type) {
  char *r = NULL;
  int fm = fan && fan[0] ? fan_mode(fan) : -1;
  int wm = water && water[0] ? water_mode(water) : -1;
  if (fm > 0) {
    char p[32];
    snprintf(p, sizeof p, "[%d]", fm);
    miio_call(m, "set_custom_mode", p, &r); free(r); r = NULL;
  }
  if (wm > 0) {
    char p[32];
    snprintf(p, sizeof p, "[%d]", wm);
    miio_call(m, "set_water_box_custom_mode", p, &r); free(r); r = NULL;
  }
  if (cycles < 1) cycles = 1;
  if (cycles > 3) cycles = 3;
  /* type: auto|spot — spot uses app_spot once */
  if (type && !strcmp(type, "spot")) {
    return miio_call(m, "app_spot", "[]", &r) == 0 ? (free(r), 0) : -1;
  }
  /* set clean count if supported, then start */
  {
    char p[16];
    snprintf(p, sizeof p, "[%d]", cycles);
    miio_call(m, "set_clean_count", p, &r); free(r); r = NULL;
  }
  return miio_call(m, "app_start", "[]", &r) == 0 ? (free(r), 0) : -1;
}

/* Forward: defined with async act helpers below */
static int spawn_async_act(miio_client *m, const char *kind, const char *arg);

/* Lightweight schedule: /mnt/data/rockctl/schedule.json
 * {"jobs":[{"id":"morning","enabled":true,"hh":9,"mm":0,"dow":"1-5","type":"auto","cycles":1,"fan":"balanced","water":"medium"}]}
 * dow: 0=Sun .. 6=Sat or ranges like 1-5; empty = every day
 * Accept thread only stamps + queues start — never blocks on miio.
 */
static void schedule_tick(miio_client *m) {
  static time_t last_min = 0;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  time_t this_min = now / 60;
  if (this_min == last_min) return;
  last_min = this_min;

  FILE *f = fopen("/mnt/data/rockctl/schedule.json", "r");
  if (!f) return;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  if (!strstr(buf, "\"jobs\"")) return;

  /* fire once per job per day: stamp in schedule.last */
  char stamp[64];
  snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
  char last[64];
  read_small("/mnt/data/rockctl/schedule.last", last, sizeof last, "");

  /* naive scan: for each "hh":N near matching hour */
  const char *p = buf;
  while ((p = strstr(p, "\"id\"")) != NULL) {
    char id[48] = {0}, fan[24] = {0}, water[24] = {0}, type[16] = {0}, dow[32] = {0};
    int hh = -1, mm = -1, cycles = 1, enabled = 1;
    const char *blk = p;
    const char *next = strstr(p + 4, "\"id\"");
    size_t blen = next ? (size_t)(next - blk) : strlen(blk);
    char chunk[1024];
    if (blen >= sizeof chunk) blen = sizeof chunk - 1;
    memcpy(chunk, blk, blen); chunk[blen] = 0;

    json_str(chunk, "id", id, sizeof id);
    json_str(chunk, "fan", fan, sizeof fan);
    json_str(chunk, "water", water, sizeof water);
    json_str(chunk, "type", type, sizeof type);
    json_str(chunk, "dow", dow, sizeof dow);
    {
      const char *e = strstr(chunk, "\"enabled\"");
      if (e) {
        e = strchr(e, ':');
        if (e) {
          while (*e && (*e == ':' || *e == ' ')) e++;
          if (!strncmp(e, "false", 5) || e[0] == '0') enabled = 0;
        }
      }
    }
    {
      const char *h = strstr(chunk, "\"hh\"");
      if (h) { h = strchr(h, ':'); if (h) hh = atoi(h + 1); }
    }
    {
      const char *h = strstr(chunk, "\"mm\"");
      if (h) { h = strchr(h, ':'); if (h) mm = atoi(h + 1); }
    }
    {
      const char *h = strstr(chunk, "\"cycles\"");
      if (h) { h = strchr(h, ':'); if (h) cycles = atoi(h + 1); }
    }
    if (!enabled || !id[0] || hh < 0 || mm < 0) { p += 4; continue; }
    if (hh != tm.tm_hour || mm != tm.tm_min) { p += 4; continue; }

    /* day-of-week filter */
    if (dow[0]) {
      int want = 0;
      int d = tm.tm_wday;
      /* parse simple lists 1,2,3 or range 1-5 */
      const char *s = dow;
      while (*s) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        int a = atoi(s);
        while (*s && *s != ',' && *s != '-') s++;
        if (*s == '-') {
          s++;
          int b = atoi(s);
          while (*s && *s != ',') s++;
          if (d >= a && d <= b) want = 1;
        } else {
          if (d == a) want = 1;
        }
      }
      if (!want) { p += 4; continue; }
    }

    char mark[96];
    snprintf(mark, sizeof mark, "%s|%s", stamp, id);
    if (!strcmp(last, mark)) { p += 4; continue; }
    write_small("/mnt/data/rockctl/schedule.last", mark);
    if (!type[0]) snprintf(type, sizeof type, "auto");
    /* Fire clean via async control start — never multi-miio on accept thread. */
    spawn_async_act(m, "control", "start");
    p += 4;
  }
}


/* Detached async manual: client already got 202; we run miio off-request. */
typedef struct {
  char *body_copy;
  char host[64];
  int miio_port;
  char token_path[160];
} async_man_job;

static void *rock_async_manual_worker(void *arg) {
  async_man_job *job = (async_man_job *)arg;
  miio_client mc;
  char *r = NULL;
  if (!job) return NULL;
  memset(&mc, 0, sizeof mc);
  mc.sock = -1;
  if (miio_prepare(&mc, job->host, job->miio_port, job->token_path) == 0)
    do_manual(&mc, job->body_copy ? job->body_copy : "", &r);
  free(r);
  miio_close(&mc);
  free(job->body_copy);
  free(job);
  return NULL;
}

/* Generic async control/fan/water — 202 then miio off-thread. */
typedef struct {
  char kind[16]; /* control | fan | water */
  char arg[48];
  char host[64];
  int miio_port;
  char token_path[160];
} async_act_job;

static void *rock_async_act_worker(void *arg) {
  async_act_job *job = (async_act_job *)arg;
  miio_client mc;
  char *r = NULL;
  if (!job) return NULL;
  memset(&mc, 0, sizeof mc);
  mc.sock = -1;
  if (miio_prepare(&mc, job->host, job->miio_port, job->token_path) == 0) {
    if (!strcmp(job->kind, "control"))
      do_action(&mc, job->arg, &r);
    else if (!strcmp(job->kind, "fan")) {
      int mode = fan_mode(job->arg);
      if (mode >= 0) {
        char params[32];
        snprintf(params, sizeof params, "[%d]", mode);
        miio_call(&mc, "set_custom_mode", params, &r);
      }
    } else if (!strcmp(job->kind, "water")) {
      int mode = water_mode(job->arg);
      if (mode >= 0) {
        char params[32];
        snprintf(params, sizeof params, "[%d]", mode);
        miio_call(&mc, "set_water_box_custom_mode", params, &r);
      }
    }
  }
  free(r);
  miio_close(&mc);
  free(job);
  return NULL;
}

static int spawn_async_act(miio_client *m, const char *kind, const char *arg) {
  async_act_job *job;
  pthread_t th;
  job = (async_act_job *)calloc(1, sizeof *job);
  if (!job) return -1;
  snprintf(job->kind, sizeof job->kind, "%s", kind ? kind : "control");
  snprintf(job->arg, sizeof job->arg, "%s", arg ? arg : "");
  snprintf(job->host, sizeof job->host, "%s",
           (m && m->host[0]) ? m->host : "127.0.0.1");
  job->miio_port = (m && m->port > 0) ? m->port : 54321;
  snprintf(job->token_path, sizeof job->token_path, "%s",
           (m && m->token_path[0]) ? m->token_path : "/mnt/data/miio/device.token");
  if (pthread_create(&th, NULL, rock_async_act_worker, job) == 0) {
    pthread_detach(th);
    return 0;
  }
  rock_async_act_worker(job);
  return 0;
}

/* Status cache + single-flight background refresh (stale-while-revalidate). */
enum { ST_TTL_S = 2, ST_STALE_S = 30 };
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_st_buf[6144];
static time_t g_st_ts;
static int g_st_have;
static int g_st_refreshing;
static char g_st_host[64];
static int g_st_port;
static char g_st_token[160];

static void status_cache_store(const char *json) {
  size_t n;
  if (!json || !json[0]) return;
  n = strlen(json);
  if (n >= sizeof g_st_buf) n = sizeof g_st_buf - 1;
  pthread_mutex_lock(&g_st_mu);
  memcpy(g_st_buf, json, n);
  g_st_buf[n] = 0;
  g_st_ts = time(NULL);
  g_st_have = 1;
  pthread_mutex_unlock(&g_st_mu);
}

static void *status_bg_refresh(void *arg) {
  miio_client mc;
  char *r = NULL;
  (void)arg;
  memset(&mc, 0, sizeof mc);
  mc.sock = -1;
  if (miio_prepare(&mc, g_st_host[0] ? g_st_host : "127.0.0.1",
                   g_st_port > 0 ? g_st_port : 54321,
                   g_st_token[0] ? g_st_token : "/mnt/data/miio/device.token") == 0) {
    mc.timeout_ms = 1200;
    if (miio_call(&mc, "get_status", "[]", &r) == 0 && r)
      status_cache_store(r);
    free(r);
  }
  miio_close(&mc);
  pthread_mutex_lock(&g_st_mu);
  g_st_refreshing = 0;
  pthread_mutex_unlock(&g_st_mu);
  return NULL;
}

static void status_kick_refresh(miio_client *m) {
  pthread_t th;
  pthread_mutex_lock(&g_st_mu);
  if (g_st_refreshing) {
    pthread_mutex_unlock(&g_st_mu);
    return;
  }
  g_st_refreshing = 1;
  if (m) {
    snprintf(g_st_host, sizeof g_st_host, "%s", m->host[0] ? m->host : "127.0.0.1");
    g_st_port = m->port > 0 ? m->port : 54321;
    snprintf(g_st_token, sizeof g_st_token, "%s",
             m->token_path[0] ? m->token_path : "/mnt/data/miio/device.token");
  }
  pthread_mutex_unlock(&g_st_mu);
  if (pthread_create(&th, NULL, status_bg_refresh, NULL) == 0)
    pthread_detach(th);
  else {
    status_bg_refresh(NULL);
  }
}

static void handle(int cfd, miio_client *m) {
  size_t req_len = 0;
  char *req = read_req(cfd, &req_len);
  if (!req) { close(cfd); return; }
  int is_get = !strncmp(req,"GET ",4) || !strncmp(req,"HEAD ",5);
  int is_post = !strncmp(req,"POST ",5);
  int is_put = !strncmp(req,"PUT ",4);
  int is_del = !strncmp(req,"DELETE ",7);
  int is_options = !strncmp(req,"OPTIONS ",8);
  const char *sp = strchr(req,' ');
  char path[128]={0};
  if (sp) {
    sp++; size_t i=0;
    while (sp[i] && sp[i]!=' ' && sp[i]!='?' && i+1<sizeof path) { path[i]=sp[i]; i++; }
    path[i]=0;
  }

  if (is_options) {
    resp_cors_preflight(cfd);
    free(req);
    close(cfd);
    return;
  }

  if (is_get && !strcmp(path,"/api/v1/health")) {
    resp(cfd,200,"application/json",
         "{\"ok\":true,\"service\":\"rockctl\",\"version\":\"0.1.2-nb\"}");
  } else if (is_get && !strcmp(path,"/api/v1/status")) {
    /* Non-blocking SWR: always answer from cache when possible; refresh in bg. */
    time_t now = time(NULL);
    int age = -1;
    char local[6144];
    int have = 0;

    pthread_mutex_lock(&g_st_mu);
    if (g_st_have && g_st_buf[0]) {
      size_t n = strlen(g_st_buf);
      if (n >= sizeof local) n = sizeof local - 1;
      memcpy(local, g_st_buf, n);
      local[n] = 0;
      have = 1;
      age = (int)(now - g_st_ts);
    }
    pthread_mutex_unlock(&g_st_mu);

    if (have && age >= 0 && age <= ST_TTL_S) {
      resp(cfd, 200, "application/json", local);
    } else if (have && age >= 0 && age <= ST_STALE_S) {
      /* stale but usable — kick refresh, never block Dash */
      if (age > ST_TTL_S) status_kick_refresh(m);
      resp(cfd, 200, "application/json", local);
    } else {
      /* cold cache: one sync fill so first paint works, then stay warm */
      char *r = NULL;
      if (m) m->timeout_ms = 1200;
      if (miio_call(m, "get_status", "[]", &r) == 0 && r) {
        status_cache_store(r);
        resp(cfd, 200, "application/json", r);
        free(r);
      } else {
        free(r);
        status_kick_refresh(m);
        if (have)
          resp(cfd, 200, "application/json", local);
        else
          resp(cfd, 503, "application/json",
               "{\"error\":\"status warming\",\"hint\":\"retry in 1s\"}");
      }
    }
  } else if (is_get && !strcmp(path,"/api/v1/consumable")) {
    char *r=NULL;
    if (miio_call(m,"get_consumable","[]",&r)!=0) resp(cfd,502,"application/json","{\"error\":\"miio call failed\"}");
    else { resp(cfd,200,"application/json",r); free(r); }
  } else if ((is_post||is_put) && !strcmp(path,"/api/v1/control")) {
    char *body = strstr(req,"\r\n\r\n"); body = body?body+4:"";
    char action[32];
    int want_sync = body && (strstr(body, "\"sync\":true") || strstr(body, "\"sync\":1"));
    if (!json_str(body,"action",action,sizeof action)) {
      resp(cfd,400,"application/json","{\"error\":\"missing action\"}");
    } else if (!want_sync) {
      /* Default async — Dash stays snappy; miio runs off-thread */
      if (spawn_async_act(m, "control", action) != 0)
        resp(cfd,500,"application/json","{\"error\":\"spawn failed\"}");
      else
        resp(cfd,202,"application/json",
             "{\"ok\":true,\"async\":true,\"action\":\"done\",\"queued\":true}");
      /* invalidate status cache so next poll refreshes */
      pthread_mutex_lock(&g_st_mu);
      g_st_ts = 0;
      pthread_mutex_unlock(&g_st_mu);
      status_kick_refresh(m);
    } else {
      char *r=NULL; int rc=do_action(m,action,&r);
      if (rc==-2) resp(cfd,400,"application/json","{\"error\":\"unknown action\"}");
      else if (rc!=0) resp(cfd,502,"application/json","{\"error\":\"miio call failed\"}");
      else { resp(cfd,200,"application/json",r?r:"{\"result\":[\"ok\"]}"); free(r); }
      pthread_mutex_lock(&g_st_mu);
      g_st_ts = 0;
      pthread_mutex_unlock(&g_st_mu);
    }
  } else if ((is_post||is_put) &&
             (!strcmp(path,"/api/v1/manual") || !strcmp(path,"/api/v1/rc") ||
              !strcmp(path,"/api/v1/manual/async") || !strcmp(path,"/api/v1/rc/async"))) {
    /* Manual drive. async=true or /manual/async → 202 + detached miio (non-blocking client). */
    char *body = strstr(req,"\r\n\r\n"); body = body?body+4:"";
    int want_async = !strcmp(path,"/api/v1/manual/async") || !strcmp(path,"/api/v1/rc/async") ||
                     (body && (strstr(body, "\"async\":true") || strstr(body, "\"async\":1") ||
                               strstr(body, "\"async\": true")));
    if (want_async) {
      async_man_job *job = (async_man_job *)calloc(1, sizeof *job);
      size_t blen = body ? strlen(body) : 0;
      pthread_t th;
      if (!job || !(job->body_copy = (char *)malloc(blen + 1))) {
        free(job);
        resp(cfd,500,"application/json","{\"error\":\"oom\"}");
      } else {
        memcpy(job->body_copy, body ? body : "", blen + 1);
        snprintf(job->host, sizeof job->host, "%s",
                 (m && m->host[0]) ? m->host : "127.0.0.1");
        job->miio_port = (m && m->port > 0) ? m->port : 54321;
        snprintf(job->token_path, sizeof job->token_path, "%s",
                 (m && m->token_path[0]) ? m->token_path : "/mnt/data/miio/device.token");
        resp(cfd,202,"application/json",
             "{\"ok\":true,\"async\":true,\"mode\":\"manual\"}");
        if (pthread_create(&th, NULL, rock_async_manual_worker, job) == 0)
          pthread_detach(th);
        else {
          rock_async_manual_worker(job);
        }
      }
    } else {
      char *r=NULL; int rc=do_manual(m, body?body:"", &r);
      if (rc==-2)
        resp(cfd,400,"application/json",
             "{\"error\":\"action start|stop|move|forward|back|left|right|halt\","
             "\"note\":\"use async:true or /api/v1/manual/async for non-blocking\"}");
      else if (rc!=0)
        resp(cfd,502,"application/json","{\"error\":\"miio manual failed\"}");
      else {
        char wrap[768];
        const char *mi = r ? r : "{\"result\":[\"ok\"]}";
        int n = snprintf(wrap, sizeof wrap,
                         "{\"ok\":true,\"mode\":\"manual\",\"miio\":%s}",
                         (mi[0]=='{'||mi[0]=='[') ? mi : "null");
        if (n > 0 && n < (int)sizeof wrap) resp(cfd,200,"application/json",wrap);
        else resp(cfd,200,"application/json","{\"ok\":true,\"mode\":\"manual\"}");
        free(r);
      }
    }
  } else if (is_post && (!strcmp(path,"/api/v1/demo/work-room") || !strcmp(path,"/api/v1/demo/work_room"))) {
    char *tmp=NULL;
    miio_call(m,"app_stop","[]",&tmp); free(tmp); tmp=NULL;
    miio_call(m,"app_pause","[]",&tmp); free(tmp);
    int x=30000, y=28000;
    FILE *pf=fopen("/mnt/data/rockctl/places.json","r");
    if(!pf) pf=fopen("/tmp/rockctl_places.json","r");
    if(pf){
      char buf[8192]; size_t n=fread(buf,1,sizeof buf-1,pf); fclose(pf); buf[n]=0;
      const char *w=strstr(buf,"\"work_room\"");
      if(w){
        const char *px=strstr(w,"\"x\"");
        const char *py=strstr(w,"\"y\"");
        if(px){ const char *c=strchr(px,':'); if(c) x=atoi(c+1); }
        if(py){ const char *c=strchr(py,':'); if(c) y=atoi(c+1); }
      }
    }
    char params[64];
    snprintf(params,sizeof params,"[%d,%d]",x,y);
    char *r=NULL;
    if(miio_call(m,"app_goto_target",params,&r)!=0){
      resp(cfd,502,"application/json","{\"error\":\"goto failed\",\"demo\":\"work-room\"}");
    } else {
      char *esc = r ? r : "{}";
      char body[1024];
      /* result may be JSON object — embed as string if needed */
      int n=snprintf(body,sizeof body,
        "{\"ok\":true,\"demo\":\"work-room\",\"clean\":false,\"x\":%d,\"y\":%d,\"miio\":%s}",
        x,y, esc[0]=='{'||esc[0]=='[' ? esc : "null");
      if(n>0 && n<(int)sizeof body) resp(cfd,200,"application/json",body);
      else resp(cfd,200,"application/json","{\"ok\":true,\"demo\":\"work-room\"}");
      free(r);
    }
  } else if (is_post && !strcmp(path,"/api/v1/goto")) {
    char *body = strstr(req,"\r\n\r\n"); body=body?body+4:"";
    /* naive "x":N "y":N */
    int x=0,y=0;
    const char *px=strstr(body,"\"x\""); const char *py=strstr(body,"\"y\"");
    if(px){ const char *c=strchr(px,':'); if(c) x=atoi(c+1); }
    if(py){ const char *c=strchr(py,':'); if(c) y=atoi(c+1); }
    if(!px||!py){ resp(cfd,400,"application/json","{\"error\":\"need x,y\"}"); }
    else {
      char *tmp=NULL; miio_call(m,"app_stop","[]",&tmp); free(tmp); tmp=NULL;
      miio_call(m,"app_pause","[]",&tmp); free(tmp);
      char params[64]; snprintf(params,sizeof params,"[%d,%d]",x,y);
      char *r=NULL;
      if(miio_call(m,"app_goto_target",params,&r)!=0)
        resp(cfd,502,"application/json","{\"error\":\"goto failed\"}");
      else { resp(cfd,200,"application/json",r?r:"{}"); free(r); }
    }
  } else if (is_post && !strcmp(path,"/api/v1/zone_clean")) {
    /* Small zone clean around a miio-frame point (app_zoned_clean). */
    char *body = strstr(req,"\r\n\r\n"); body=body?body+4:"";
    int x=0,y=0, size=1500, repeats=1;
    const char *px=strstr(body,"\"x\""); const char *py=strstr(body,"\"y\"");
    const char *ps=strstr(body,"\"size_mm\""); const char *pr=strstr(body,"\"repeats\"");
    if(px){ const char *c=strchr(px,':'); if(c) x=atoi(c+1); }
    if(py){ const char *c=strchr(py,':'); if(c) y=atoi(c+1); }
    if(ps){ const char *c=strchr(ps,':'); if(c) size=atoi(c+1); }
    if(pr){ const char *c=strchr(pr,':'); if(c) repeats=atoi(c+1); }
    if(!px||!py){ resp(cfd,400,"application/json","{\"error\":\"need x,y (miio/goto frame)\"}"); }
    else {
      if(size < 500) size=500; if(size>4000) size=4000;
      if(repeats<1) repeats=1; if(repeats>3) repeats=3;
      int half=size/2;
      int x1=x-half, y1=y-half, x2=x+half, y2=y+half;
      char params[96];
      snprintf(params,sizeof params,"[[%d,%d,%d,%d,%d]]", x1,y1,x2,y2,repeats);
      char *tmp=NULL; miio_call(m,"app_stop","[]",&tmp); free(tmp); tmp=NULL;
      miio_call(m,"app_pause","[]",&tmp); free(tmp);
      char *r=NULL;
      if(miio_call(m,"app_zoned_clean",params,&r)!=0)
        resp(cfd,502,"application/json","{\"error\":\"zone clean failed\"}");
      else {
        char out[256];
        snprintf(out,sizeof out,
          "{\"ok\":true,\"method\":\"app_zoned_clean\",\"zone\":[%d,%d,%d,%d],\"repeats\":%d,\"miio\":%s}",
          x1,y1,x2,y2,repeats, (r&&r[0]=='{')?r:"{}");
        resp(cfd,200,"application/json",out);
        free(r);
      }
    }
  } else if (is_put && !strcmp(path,"/api/v1/fan")) {
    char *body = strstr(req,"\r\n\r\n"); body = body?body+4:"";
    char level[32];
    int want_sync = body && (strstr(body, "\"sync\":true") || strstr(body, "\"sync\":1"));
    if (!json_str(body,"level",level,sizeof level)) {
      resp(cfd,400,"application/json","{\"error\":\"missing level\"}");
    } else if (fan_mode(level) < 0) {
      resp(cfd,400,"application/json","{\"error\":\"bad level\"}");
    } else if (!want_sync) {
      spawn_async_act(m, "fan", level);
      resp(cfd,202,"application/json","{\"ok\":true,\"async\":true,\"fan\":true}");
    } else {
      int mode = fan_mode(level);
      char params[32]; snprintf(params,sizeof params,"[%d]", mode);
      char *r=NULL;
      if (miio_call(m,"set_custom_mode",params,&r)!=0)
        resp(cfd,502,"application/json","{\"error\":\"miio call failed\"}");
      else { resp(cfd,200,"application/json",r?r:"{}"); free(r); }
    }
  } else if (is_post && !strcmp(path,"/api/v1/raw")) {
    char *body = strstr(req,"\r\n\r\n"); body = body?body+4:"";
    char method[64];
    if (!json_str(body,"method",method,sizeof method)) {
      resp(cfd,400,"application/json","{\"error\":\"missing method\"}");
    } else {
      /* optional params as raw JSON after "params": */
      const char *params = "[]";
      const char *pp = strstr(body, "\"params\"");
      char pbuf[512];
      if (pp) {
        pp = strchr(pp, ':');
        if (pp) {
          pp++;
          while (*pp==' '||*pp=='\t'||*pp=='\n') pp++;
          /* copy until end-ish */
          size_t i=0;
          int depth=0; int started=0;
          while (*pp && i+1<sizeof pbuf) {
            char ch=*pp++;
            pbuf[i++]=ch;
            if (ch=='['||ch=='{'){ depth++; started=1; }
            else if (ch==']'||ch=='}'){ depth--; if(started && depth==0) break; }
          }
          pbuf[i]=0; params=pbuf;
        }
      }
      char *r=NULL;
      if (miio_call(m, method, params, &r)!=0)
        resp(cfd,502,"application/json","{\"error\":\"miio call failed\"}");
      else { resp(cfd,200,"application/json",r?r:"{}"); free(r); }
    }
  } else if (is_get && (!strcmp(path,"/api/v1/drive/last") || !strcmp(path,"/api/v1/drive/path"))) {
    /* Last drive path for ClankerDash coverage + lab export */
    char *body = (char*)malloc(512*1024);
    int n;
    if (!body) { resp(cfd,500,"application/json","{\"error\":\"oom\"}"); }
    else {
      n = drive_path_read_last(body, 512*1024);
      if (n < 0) resp(cfd,404,"application/json","{\"error\":\"no path\"}");
      else resp(cfd,200,"application/json",body);
      free(body);
    }
  } else if ((is_post || is_put) && (!strcmp(path,"/api/v1/drive/last") || !strcmp(path,"/api/v1/drive/path"))) {
    /* Teach/lab posts full session JSON */
    char *dbody = strstr(req,"\r\n\r\n"); dbody = dbody ? dbody+4 : "";
    if (!dbody || !dbody[0]) resp(cfd,400,"application/json","{\"error\":\"empty body\"}");
    else if (drive_path_write_last(dbody, strlen(dbody)) != 0)
      resp(cfd,500,"application/json","{\"error\":\"write failed\"}");
    else resp(cfd,200,"application/json","{\"ok\":true,\"saved\":\"/mnt/data/rockctl/drive/last.json\"}");
  } else if (is_get && (!strcmp(path,"/api/v1/drive/coverage") || !strcmp(path,"/api/v1/drive/map_path"))) {
    /* RRSLAM path layer → points (last clean coverage) */
    char *cbody = (char*)malloc(768*1024);
    int n;
    if (!cbody) { resp(cfd,500,"application/json","{\"error\":\"oom\"}"); }
    else {
      n = drive_path_extract_rrslam(cbody, 768*1024, 1200);
      if (n < 0) resp(cfd,500,"application/json","{\"error\":\"extract failed\"}");
      else resp(cfd,200,"application/json",cbody);
      free(cbody);
    }
  } else if (is_get && !strcmp(path,"/api/v1/maps")) {

    const char *names[] = {"last_map","user_map0",NULL};
    char body[3072];
    size_t o = 0;
    o += (size_t)snprintf(body+o, sizeof body-o, "{\"ok\":true,\"format\":\"RRSLAM\",\"maps\":[");
    int first = 1;
    int mw=0, mh=0, moff=46, mtop=0, mleft=0;
    for (int i=0; names[i]; i++) {
      char pathm[256];
      snprintf(pathm, sizeof pathm, "/mnt/data/rockrobo/%s", names[i]);
      struct stat st;
      if (stat(pathm, &st) != 0) continue;
      if (first && !strcmp(names[i], "last_map")) {
        char *bin=NULL; size_t bl=0;
        if (read_file_bin(pathm, &bin, &bl) == 0 && bin) {
          rrslam_dims(bin, bl, &mw, &mh, &moff, &mtop, &mleft);
          free(bin);
        }
      }
      o += (size_t)snprintf(body+o, sizeof body-o,
        "%s{\"name\":\"%s\",\"bytes\":%ld}",
        first?"":",", names[i], (long)st.st_size);
      first = 0;
    }
    int cx=0,cy=0,cok=0, sx=0,sy=0,sok=0;
    parse_xy_file("/mnt/data/rockrobo/ChargerPos.data", &cx, &cy, &cok);
    parse_xy_file("/mnt/data/rockrobo/StartPos.data", &sx, &sy, &sok);
    o += (size_t)snprintf(body+o, sizeof body-o, "]");
    const int res_mm = 50;
    /* Pixel (0,0) top-left: slam_mm = (left + px)*res , (top + py)*res */
    int ox = mleft * res_mm;
    int oy = mtop * res_mm;
    const int goto_off = 25500;
    o += (size_t)snprintf(body+o, sizeof body-o,
      ",\"width\":%d,\"height\":%d,\"data_offset\":%d,\"resolution_mm\":%d"
      ",\"map_top_cell\":%d,\"map_left_cell\":%d"
      ",\"origin_mm\":{\"x\":%d,\"y\":%d}"
      ",\"y_axis\":\"down\""
      ",\"coord_frame\":\"slam\""
      ",\"goto_offset_mm\":{\"x\":%d,\"y\":%d}"
      ",\"goto_frame\":\"miio_app_goto_target\"",
      mw, mh, moff, res_mm, mtop, mleft, ox, oy, goto_off, goto_off);
    if (cok) o += (size_t)snprintf(body+o, sizeof body-o,
      ",\"charger\":{\"x\":%d,\"y\":%d}"
      ",\"charger_goto\":{\"x\":%d,\"y\":%d}",
      cx, cy, cx + goto_off, cy + goto_off);
    if (sok) o += (size_t)snprintf(body+o, sizeof body-o,
      ",\"start\":{\"x\":%d,\"y\":%d}", sx, sy);
    o += (size_t)snprintf(body+o, sizeof body-o, "}");
    resp(cfd,200,"application/json",body);
  } else if (is_get && !strncmp(path,"/api/v1/maps/", 13)) {
    /* Raw map bytes only on the vacuum (light). 3D OBJ/PLY is offloaded. */
    const char *rest = path + 13;
    if (strchr(rest,'/') || strchr(rest,'\\') || !rest[0]) {
      resp(cfd,400,"application/json","{\"error\":\"bad map name\"}");
    } else {
      char name[64];
      const char *dot = strrchr(rest, '.');
      int want3d = 0;
      if (dot && (!strcmp(dot,".obj") || !strcmp(dot,".ply"))) {
        want3d = 1;
        size_t nl = (size_t)(dot - rest);
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, rest, nl); name[nl] = 0;
      } else if (dot && (!strcmp(dot,".bin") || !strcmp(dot,".rrslam"))) {
        size_t nl = (size_t)(dot - rest);
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, rest, nl); name[nl] = 0;
      } else {
        snprintf(name, sizeof name, "%s", rest);
      }
      if (want3d) {
        resp(cfd,501,"application/json",
          "{\"error\":\"3d export offloaded — use ClankerDash Export 3D (runs on your PC/browser) or map_3d_worker on BlackCube/Groot\",\"offload\":true}");
      } else if (strcmp(name,"last_map") && strcmp(name,"user_map0")) {
        resp(cfd,404,"application/json","{\"error\":\"unknown map\"}");
      } else {
        char pathm[256];
        snprintf(pathm, sizeof pathm, "/mnt/data/rockrobo/%s", name);
        char *bin=NULL; size_t bl=0;
        if (read_file_bin(pathm, &bin, &bl) != 0) {
          resp(cfd,404,"application/json","{\"error\":\"map not found\"}");
        } else {
          char fname[64];
          snprintf(fname, sizeof fname, "%s.bin", name);
          resp_bin(cfd, 200, "application/octet-stream", fname, bin, bl);
          free(bin);
        }
      }
    }

  } else if (is_get && !strcmp(path,"/api/v1/music")) {
    mkdir("/mnt/data/rockctl", 0755);
    mkdir("/mnt/data/rockctl/music", 0755);
    char mode[32], pmode[32], cur[96];
    read_small("/mnt/data/rockctl/music_mode", mode, sizeof mode, "clean");
    read_small("/mnt/data/rockctl/play_mode", pmode, sizeof pmode, "loop");
    read_small("/mnt/data/rockctl/current", cur, sizeof cur, "");
    int playing = 0;
    FILE *pf = fopen("/mnt/data/clean_music_aplay.pid", "r");
    if (pf) {
      int pid=0; if (fscanf(pf, "%d", &pid)==1 && pid>1) {
        char ppath[64]; snprintf(ppath,sizeof ppath,"/proc/%d", pid);
        playing = (access(ppath, F_OK)==0);
      }
      fclose(pf);
    }
    /* list tracks */
    char tracks[2048]; size_t to=0; tracks[0]=0;
    to += (size_t)snprintf(tracks+to, sizeof tracks-to, "[");
    int first=1;
    DIR *d = opendir("/mnt/data/rockctl/music");
    if (d) {
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 5) continue;
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcasecmp(dot, ".wav") != 0) continue;
        char pathm[256]; snprintf(pathm,sizeof pathm,"/mnt/data/rockctl/music/%s", de->d_name);
        struct stat st; long bytes=0;
        if (stat(pathm,&st)==0) bytes=(long)st.st_size;
        if (to + 96 >= sizeof tracks) break;
        to += (size_t)snprintf(tracks+to, sizeof tracks-to,
          "%s{\"name\":\"%s\",\"bytes\":%ld}", first?"":",", de->d_name, bytes);
        first=0;
      }
      closedir(d);
    }
    /* legacy fallbacks if empty */
    if (first) {
      const char *leg[] = {"/mnt/data/clean_loop_full.wav","/mnt/data/clean_loop_30s.wav",NULL};
      for (int i=0; leg[i]; i++) {
        struct stat st; if (stat(leg[i],&st)!=0) continue;
        const char *bn = strrchr(leg[i],'/'); bn = bn?bn+1:leg[i];
        to += (size_t)snprintf(tracks+to, sizeof tracks-to,
          "%s{\"name\":\"%s\",\"bytes\":%ld,\"legacy\":true}", first?"":",", bn, (long)st.st_size);
        first=0;
      }
    }
    to += (size_t)snprintf(tracks+to, sizeof tracks-to, "]");
    char body[3072];
    snprintf(body, sizeof body,
      "{\"ok\":true,\"mode\":\"%s\",\"play_mode\":\"%s\",\"playing\":%s,"
      "\"current\":\"%s\",\"tracks\":%s,"
      "\"dir\":\"/mnt/data/rockctl/music\","
      "\"limits\":{\"max_upload_bytes\":%d,\"formats\":[\"wav\"]}}",
      mode[0]?mode:"clean", pmode[0]?pmode:"loop", playing?"true":"false",
      cur, tracks, ROCK_HTTP_MAX_BODY);
    resp(cfd,200,"application/json",body);

  } else if ((is_post||is_put) && !strcmp(path,"/api/v1/music")) {
    char *body = strstr(req,"\r\n\r\n"); body=body?body+4:"";
    mkdir("/mnt/data/rockctl", 0755);
    mkdir("/mnt/data/rockctl/music", 0755);
    char mode[32]={0}, pmode[32]={0}, act[32]={0}, track[96]={0};
    json_str(body,"mode",mode,sizeof mode);
    if (!json_str(body,"play_mode",pmode,sizeof pmode))
      json_str(body,"playback",pmode,sizeof pmode);
    json_str(body,"action",act,sizeof act);
    json_str(body,"track",track,sizeof track);
    /* action aliases */
    if (!mode[0] && act[0]) {
      if (!strcmp(act,"play")||!strcmp(act,"on")) strcpy(mode,"on");
      else if (!strcmp(act,"stop")||!strcmp(act,"off")) strcpy(mode,"off");
      else if (!strcmp(act,"clean")||!strcmp(act,"auto")) strcpy(mode,"clean");
    }
    if (mode[0]) {
      if (!strcmp(mode,"always")) strcpy(mode,"on");
      if (!strcmp(mode,"auto")) strcpy(mode,"clean");
      if (strcmp(mode,"off")&&strcmp(mode,"clean")&&strcmp(mode,"on")) {
        resp(cfd,400,"application/json","{\"error\":\"mode off|clean|on\"}");
        free(req); close(cfd); return;
      }
      write_small("/mnt/data/rockctl/music_mode", mode);
      if (!strcmp(mode,"off")) {
        run_sh("kill $(cat /mnt/data/clean_music_aplay.pid 2>/dev/null) 2>/dev/null; killall aplay 2>/dev/null; true");
      }
    }
    if (pmode[0]) {
      if (!strcmp(pmode,"repeat")) strcpy(pmode,"loop");
      if (!strcmp(pmode,"random")) strcpy(pmode,"shuffle");
      if (!strcmp(pmode,"one")||!strcmp(pmode,"single")) strcpy(pmode,"once");
      if (strcmp(pmode,"once")&&strcmp(pmode,"loop")&&strcmp(pmode,"shuffle")) {
        resp(cfd,400,"application/json","{\"error\":\"play_mode once|loop|shuffle\"}");
        free(req); close(cfd); return;
      }
      write_small("/mnt/data/rockctl/play_mode", pmode);
      run_sh("rm -f /tmp/clanker_music_once_done");
    }
    if (track[0]) {
      char safe[96];
      if (safe_track_name(track, safe, sizeof safe)==0) {
        char full[256];
        snprintf(full,sizeof full,"/mnt/data/rockctl/music/%s", safe);
        if (access(full, R_OK)==0) {
          write_small("/mnt/data/rockctl/current", safe);
          write_small("/mnt/data/rockctl/current.path", full);
        }
      }
    }
    if (!strcmp(act,"next") || !strcmp(act,"prev") || !strcmp(act,"skip")) {
      /* stop current; watchdog picks next based on play_mode */
      run_sh("kill $(cat /mnt/data/clean_music_aplay.pid 2>/dev/null) 2>/dev/null; killall aplay 2>/dev/null; "
             "rm -f /tmp/clanker_music_once_done; true");
      if (!strcmp(act,"prev")) {
        /* simple: clear current so loop restarts from first; shuffle randomizes */
        run_sh("rm -f /mnt/data/rockctl/current /mnt/data/rockctl/current.path");
      }
    }
    if (!strcmp(act,"play") || (mode[0] && strcmp(mode,"off"))) {
      /* nudge mode on if play */
      if (!strcmp(act,"play")) {
        char m2[32]; read_small("/mnt/data/rockctl/music_mode", m2, sizeof m2, "clean");
        if (!strcmp(m2,"off")) write_small("/mnt/data/rockctl/music_mode", "on");
        run_sh("rm -f /tmp/clanker_music_once_done");
      }
    }
    ensure_music_watchdog();
    char out[256];
    read_small("/mnt/data/rockctl/music_mode", mode, sizeof mode, "clean");
    read_small("/mnt/data/rockctl/play_mode", pmode, sizeof pmode, "loop");
    read_small("/mnt/data/rockctl/current", track, sizeof track, "");
    snprintf(out,sizeof out,
      "{\"ok\":true,\"mode\":\"%s\",\"play_mode\":\"%s\",\"current\":\"%s\"}",
      mode, pmode, track);
    resp(cfd,200,"application/json",out);

  } else if (is_post && !strcmp(path,"/api/v1/music/upload")) {
    /* Raw WAV body; name from ?name=song.wav or X-Filename header */
    char name[96]={0};
    const char *qline = req;
    const char *sp1 = strchr(req, ' ');
    const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp1 && sp2) {
      for (const char *q = sp1; q < sp2; q++) {
        if (q[0]=='?' || (q[0]=='n' && strncmp(q,"name=",5)==0)) {
          const char *np = strstr(q, "name=");
          if (np && np < sp2) {
            np += 5; size_t i=0;
            while (np[i] && np[i]!='&' && np[i]!=' ' && i+1<sizeof name) { name[i]=np[i]; i++; }
            name[i]=0;
          }
          break;
        }
      }
    }
    if (!name[0]) {
      const char *h = strstr(req, "X-Filename:");
      if (!h) h = strstr(req, "x-filename:");
      if (h) {
        h = strchr(h, ':');
        if (h) {
          h++;
          while (*h==' ' || *h=='\t') h++;
          size_t i=0;
          while (h[i] && h[i]!='\r' && h[i]!='\n' && i+1<sizeof name) { name[i]=h[i]; i++; }
          name[i]=0;
        }
      }
    }
    char safe[96];
    if (safe_track_name(name, safe, sizeof safe) != 0) {
      resp(cfd,400,"application/json","{\"error\":\"need safe name ending in .wav (?name=song.wav)\"}");
      free(req); close(cfd); return;
    }
    size_t cl = 0;
    const char *clh = strstr(req, "Content-Length:");
    if (!clh) clh = strstr(req, "content-length:");
    if (clh) cl = (size_t)strtoul(clh + 15, NULL, 10);
    if (cl < 44 || cl > ROCK_HTTP_MAX_BODY) {
      resp(cfd,400,"application/json","{\"error\":\"bad Content-Length (wav 44B..3MB)\"}");
      free(req); close(cfd); return;
    }
    char *hdrend = strstr(req, "\r\n\r\n");
    if (!hdrend) { resp(cfd,400,"application/json","{\"error\":\"bad request\"}"); free(req); close(cfd); return; }
    char *body = hdrend + 4;
    size_t hlen = (size_t)(body - req);
    size_t available = (req_len > hlen) ? req_len - hlen : 0;
    if (available < cl) {
      resp(cfd,400,"application/json","{\"error\":\"incomplete body\"}");
      free(req); close(cfd); return;
    }
    mkdir("/mnt/data/rockctl", 0755);
    mkdir("/mnt/data/rockctl/music", 0755);
    struct statvfs sv; memset(&sv,0,sizeof sv);
    if (statvfs("/mnt/data", &sv)==0 &&
        sv.f_bavail * (unsigned long long)sv.f_frsize < (unsigned long long)cl + 256ULL*1024ULL) {
      resp(cfd,507,"application/json","{\"error\":\"not enough free space on /mnt/data\"}");
      free(req); close(cfd); return;
    }
    char pathm[256];
    snprintf(pathm, sizeof pathm, "/mnt/data/rockctl/music/%s", safe);
    FILE *wf = fopen(pathm, "wb");
    if (!wf) { resp(cfd,500,"application/json","{\"error\":\"write failed\"}"); free(req); close(cfd); return; }
    size_t wr = fwrite(body, 1, cl, wf);
    fclose(wf);
    if (wr != cl) {
      unlink(pathm);
      resp(cfd,500,"application/json","{\"error\":\"short write\"}");
      free(req); close(cfd); return;
    }
    /* require RIFF/WAVE header so we don't store junk as .wav */
    if (cl >= 12 && !(body[0]=='R'&&body[1]=='I'&&body[2]=='F'&&body[3]=='F' &&
                      body[8]=='W'&&body[9]=='A'&&body[10]=='V'&&body[11]=='E')) {
      unlink(pathm);
      resp(cfd,400,"application/json","{\"error\":\"not a WAV file (need RIFF/WAVE)\"}");
      free(req); close(cfd); return;
    }
    write_small("/mnt/data/rockctl/current", safe);
    ensure_music_watchdog();
    char out[320];
    snprintf(out,sizeof out,
      "{\"ok\":true,\"name\":\"%s\",\"bytes\":%zu,"
      "\"limits\":{\"max_upload_bytes\":%d,\"formats\":[\"wav\"]}}",
      safe, cl, ROCK_HTTP_MAX_BODY);
    resp(cfd,200,"application/json",out);

  } else if (is_del && !strncmp(path,"/api/v1/music/tracks/", 21)) {
    const char *tn = path + 21;
    char safe[96];
    if (safe_track_name(tn, safe, sizeof safe) != 0) {
      resp(cfd,400,"application/json","{\"error\":\"bad track name\"}");
    } else {
      char pathm[256];
      snprintf(pathm,sizeof pathm,"/mnt/data/rockctl/music/%s", safe);
      if (unlink(pathm) != 0 && errno != ENOENT) {
        resp(cfd,500,"application/json","{\"error\":\"delete failed\"}");
      } else {
        char cur[96]; read_small("/mnt/data/rockctl/current", cur, sizeof cur, "");
        if (!strcmp(cur, safe)) {
          write_small("/mnt/data/rockctl/current", "");
          write_small("/mnt/data/rockctl/current.path", "");
          run_sh("kill $(cat /mnt/data/clean_music_aplay.pid 2>/dev/null) 2>/dev/null; killall aplay 2>/dev/null; true");
        }
        char out[160];
        snprintf(out,sizeof out,"{\"ok\":true,\"deleted\":\"%s\"}", safe);
        resp(cfd,200,"application/json",out);
      }
    }

  } else if ((is_post||is_put) && !strncmp(path,"/api/v1/music/tracks/", 21)) {
    /* Rename track: PUT/POST body {"name":"new.wav"} or {"new_name":"…"} */
    const char *tn = path + 21;
    char safe[96], news[96]={0};
    if (safe_track_name(tn, safe, sizeof safe) != 0) {
      resp(cfd,400,"application/json","{\"error\":\"bad track name\"}");
      free(req); close(cfd); return;
    }
    char *body = strstr(req,"\r\n\r\n"); body = body?body+4:"";
    char nn[96]={0};
    if (!json_str(body,"new_name",nn,sizeof nn))
      json_str(body,"name",nn,sizeof nn);
    if (!nn[0]) {
      resp(cfd,400,"application/json","{\"error\":\"need new_name or name (.wav)\"}");
      free(req); close(cfd); return;
    }
    if (safe_track_name(nn, news, sizeof news) != 0) {
      resp(cfd,400,"application/json","{\"error\":\"new name must be safe .wav (alnum _ - .)\"}");
      free(req); close(cfd); return;
    }
    char oldp[256], newp[256];
    snprintf(oldp,sizeof oldp,"/mnt/data/rockctl/music/%s", safe);
    snprintf(newp,sizeof newp,"/mnt/data/rockctl/music/%s", news);
    if (access(oldp, F_OK) != 0) {
      resp(cfd,404,"application/json","{\"error\":\"track not found\"}");
      free(req); close(cfd); return;
    }
    if (!strcmp(safe, news)) {
      char out0[128];
      snprintf(out0,sizeof out0,"{\"ok\":true,\"name\":\"%s\",\"unchanged\":true}", safe);
      resp(cfd,200,"application/json",out0);
      free(req); close(cfd); return;
    }
    if (access(newp, F_OK) == 0) {
      resp(cfd,409,"application/json","{\"error\":\"target name already exists\"}");
      free(req); close(cfd); return;
    }
    if (rename(oldp, newp) != 0) {
      resp(cfd,500,"application/json","{\"error\":\"rename failed\"}");
      free(req); close(cfd); return;
    }
    char cur[96]; read_small("/mnt/data/rockctl/current", cur, sizeof cur, "");
    if (!strcmp(cur, safe)) {
      write_small("/mnt/data/rockctl/current", news);
      write_small("/mnt/data/rockctl/current.path", newp);
    }
    char out[192];
    snprintf(out,sizeof out,"{\"ok\":true,\"from\":\"%s\",\"name\":\"%s\"}", safe, news);
    resp(cfd,200,"application/json",out);

  } else if (is_get && !strcmp(path,"/api/v1/config")) {
    /* Public, non-secret site profile for ClankerDash / other clients.
     * Env (optional): CLANKER_NAME, NANOBOT_PORT, ROCKCTL_PORT, NANOBOT_URL */
    const char *name = getenv("CLANKER_NAME");
    if (!name || !name[0]) name = "clanker";
    const char *np = getenv("NANOBOT_PORT");
    if (!np || !np[0]) np = "8787";
    const char *rp = getenv("ROCKCTL_PORT");
    if (!rp || !rp[0]) rp = "8080";
    const char *nurl = getenv("NANOBOT_URL");
    if (!nurl) nurl = getenv("NANOBOT_PEER_URL");
    char body[768];
    if (nurl && nurl[0]) {
      snprintf(body, sizeof body,
        "{\"ok\":true,\"name\":\"%s\",\"rockctl_port\":%s,\"nanobot_port\":%s,"
        "\"nanobot_url\":\"%s\",\"dash\":\"ClankerDash\","
        "\"endpoints\":[\"/api/v1/health\",\"/api/v1/status\",\"/api/v1/resources\",\"/api/v1/config\"],"
        "\"note\":\"set CLANKER_HOST on clients; no lab IP is embedded\"}",
        name, rp, np, nurl);
    } else {
      snprintf(body, sizeof body,
        "{\"ok\":true,\"name\":\"%s\",\"rockctl_port\":%s,\"nanobot_port\":%s,"
        "\"nanobot_url\":null,\"dash\":\"ClankerDash\","
        "\"endpoints\":[\"/api/v1/health\",\"/api/v1/status\",\"/api/v1/resources\",\"/api/v1/config\"],"
        "\"note\":\"derive nanobot URL as same host, port nanobot_port; set NANOBOT_URL to publish absolute\"}",
        name, rp, np);
    }
    resp(cfd,200,"application/json",body);

  } else if (is_get && !strcmp(path,"/api/v1/resources")) {
    long mem_total=0, mem_free=0, buffers=0, cached=0;
    FILE *mf=fopen("/proc/meminfo","r");
    if(mf){
      char line[160];
      while(fgets(line,sizeof line,mf)){
        long v;
        if(sscanf(line,"MemTotal: %ld",&v)==1) mem_total=v;
        else if(sscanf(line,"MemFree: %ld",&v)==1) mem_free=v;
        else if(sscanf(line,"Buffers: %ld",&v)==1) buffers=v;
        else if(sscanf(line,"Cached: %ld",&v)==1) cached=v;
      }
      fclose(mf);
    }
    double l1=0,l5=0,l15=0;
    FILE *lf=fopen("/proc/loadavg","r");
    if(lf){ if(fscanf(lf,"%lf %lf %lf",&l1,&l5,&l15)<1){} fclose(lf); }
    long data_total=0, data_free=0;
    struct statvfs sv; memset(&sv,0,sizeof sv);
    if(statvfs("/mnt/data",&sv)==0 && sv.f_frsize){
      data_total=(long)((sv.f_blocks*(unsigned long long)sv.f_frsize)/1024ULL);
      data_free=(long)((sv.f_bavail*(unsigned long long)sv.f_frsize)/1024ULL);
    }
    int ng_rss=0, rc_rss=0, ng_alive=0, rc_alive=0;
    /* best-effort: parse ps via /proc self only — skip heavy */
    FILE *pf=fopen("/proc/self/status","r");
    if(pf){ char line[128]; while(fgets(line,sizeof line,pf)){ int v; if(sscanf(line,"VmRSS: %d",&v)==1) rc_rss=v; } fclose(pf); rc_alive=1; }
    /* nanobot pid file */
    {
      FILE *np=fopen("/mnt/data/nanobot/nanobot.pid","r");
      int pid=0;
      if(np && fscanf(np,"%d",&pid)==1 && pid>1){
        char sp[64]; snprintf(sp,sizeof sp,"/proc/%d/status",pid);
        FILE *sf=fopen(sp,"r");
        if(sf){ ng_alive=1; char line[128]; while(fgets(line,sizeof line,sf)){ int v; if(sscanf(line,"VmRSS: %d",&v)==1) ng_rss=v; } fclose(sf); }
      }
      if(np) fclose(np);
    }
    long avail = mem_free + buffers + cached;
    char body[768];
    snprintf(body,sizeof body,
      "{\"ok\":true,\"host\":\"clanker\",\"mem_total_kb\":%ld,\"mem_free_kb\":%ld,"
      "\"mem_avail_kb\":%ld,\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
      "\"data_total_kb\":%ld,\"data_free_kb\":%ld,"
      "\"services\":{\"rockctl\":{\"alive\":true,\"rss_kb_self\":%d},"
      "\"nanobot\":{\"alive\":%s,\"rss_kb\":%d}},"
      "\"warn\":%s}",
      mem_total, mem_free, avail, l1, l5, l15, data_total, data_free,
      rc_rss, ng_alive?"true":"false", ng_rss,
      (avail < 20*1024 || data_free < 8*1024) ? "true" : "false");
    resp(cfd,200,"application/json",body);

  } else if (is_get && !strcmp(path,"/api/v1/services")) {
    char ip[48] = {0};
    int wifi = wifi_sta_up(ip, sizeof ip);
    int ng = proc_alive("/mnt/data/nanobot/nanobot.pid");
    int sh = shell_enabled_flag();
    int wa = watcher_enabled_flag();
    int ui = ui_enabled_flag();
    char www[256] = {0};
    nanobot_settings_get("WWW", www, sizeof www, "/mnt/data/nanobot-wrapper/www");
    char body[900];
    snprintf(body, sizeof body,
      "{\"ok\":true,"
      "\"rockctl\":{\"alive\":true,\"status\":\"online\"},"
      "\"assist\":{\"alive\":%s,\"status\":\"%s\",\"port\":8787},"
      "\"ui\":{\"enabled\":%s,\"www\":\"%s\",\"url_port\":8787},"
      "\"shell\":{\"enabled\":%s},"
      "\"watcher\":{\"enabled\":%s},"
      "\"wifi\":{\"sta\":%s,\"ip\":\"%s\"},"
      "\"note\":\"toggle assist|ui|shell|watcher via PUT /api/v1/services\"}",
      ng ? "true" : "false", ng ? "online" : "offline",
      ui ? "true" : "false", www[0] ? www : "",
      sh ? "true" : "false",
      wa ? "true" : "false",
      wifi ? "true" : "false", ip);
    resp(cfd, 200, "application/json", body);

  /* Peer token for nanobot assist — generate/set/status (ClankerDash manages). */
  } else if (is_get && !strcmp(path, "/api/v1/assist/peer_token")) {
    char pathf[128];
    snprintf(pathf, sizeof pathf, "/mnt/data/nanobot/peer_token");
    int has = (access(pathf, R_OK) == 0);
    char body[160];
    snprintf(body, sizeof body,
      "{\"ok\":true,\"configured\":%s,\"path\":\"/mnt/data/nanobot/peer_token\","
      "\"note\":\"token value never returned by GET — use generate/set\"}",
      has ? "true" : "false");
    resp(cfd, 200, "application/json", body);

  } else if ((is_post || is_put) && !strcmp(path, "/api/v1/assist/peer_token")) {
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    char action[32] = {0}, token[96] = {0}, master_hex[80] = {0};
    json_str(body, "action", action, sizeof action);
    json_str(body, "token", token, sizeof token);
    json_str(body, "master", master_hex, sizeof master_hex);
    if (!master_hex[0]) json_str(body, "master_hex", master_hex, sizeof master_hex);
    if (!action[0]) {
      if (token[0]) snprintf(action, sizeof action, "set");
      else snprintf(action, sizeof action, "generate");
    }
    char pathf[128];
    snprintf(pathf, sizeof pathf, "/mnt/data/nanobot/peer_token");
    if (!strcmp(action, "status")) {
      int has = (access(pathf, R_OK) == 0);
      int has_master = (access("/mnt/data/labauth/master.key", R_OK) == 0);
      char out[200];
      snprintf(out, sizeof out,
        "{\"ok\":true,\"configured\":%s,\"labauth_master\":%s,"
        "\"admin_reset\":\"action=admin_reset + master hex\"}",
        has ? "true" : "false", has_master ? "true" : "false");
      resp(cfd, 200, "application/json", out);
    } else if (!strcmp(action, "generate") || !strcmp(action, "set") ||
               !strcmp(action, "admin_reset") || !strcmp(action, "reset")) {
      /* Authz: (1) bootstrap no file (2) current peer token (3) labauth master */
      int has = (access(pathf, R_OK) == 0);
      int admin_ok = 0;
      if (has && master_hex[0]) {
        /* Admin path: master must match /mnt/data/labauth/master.key (32 raw bytes) */
        unsigned char want[32], got[32];
        int nibble = 0, ni = 0;
        memset(want, 0, sizeof want);
        for (const char *p = master_hex; *p && ni < 64; p++) {
          char c = *p;
          int v = -1;
          if (c >= '0' && c <= '9') v = c - '0';
          else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
          else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
          else continue;
          if ((ni & 1) == 0) nibble = v;
          else want[ni / 2] = (unsigned char)((nibble << 4) | v);
          ni++;
        }
        if (ni == 64) {
          int fd = open("/mnt/data/labauth/master.key", O_RDONLY);
          if (fd >= 0) {
            ssize_t n = read(fd, got, 32);
            close(fd);
            if (n == 32) {
              unsigned char d = 0;
              for (int i = 0; i < 32; i++) d |= (unsigned char)(want[i] ^ got[i]);
              admin_ok = (d == 0);
            }
          }
        }
        memset(want, 0, sizeof want);
        memset(got, 0, sizeof got);
      }
      if (has && !admin_ok) {
        char cur[96] = {0};
        {
          FILE *tf = fopen(pathf, "r");
          if (tf) {
            char line[128];
            while (fgets(line, sizeof line, tf)) {
              if (!strncmp(line, "token=", 6)) {
                snprintf(cur, sizeof cur, "%s", line + 6);
                size_t L = strlen(cur);
                while (L && (cur[L-1]=='\n'||cur[L-1]=='\r')) cur[--L]=0;
                break;
              }
              if (line[0] && line[0] != '#' && !strchr(line, '=')) {
                snprintf(cur, sizeof cur, "%s", line);
                size_t L = strlen(cur);
                while (L && (cur[L-1]=='\n'||cur[L-1]=='\r')) cur[--L]=0;
                break;
              }
            }
            fclose(tf);
          }
        }
        char presented[96] = {0};
        const char *h = strstr(req, "X-Nanobot-Peer-Token:");
        if (!h) h = strstr(req, "x-nanobot-peer-token:");
        if (!h) h = strstr(req, "X-Nanobot-Peer-Token:");
        if (h) {
          h = strchr(h, ':');
          if (h) {
            h++;
            while (*h == ' ' || *h == '\t') h++;
            size_t nl = strcspn(h, "\r\n");
            if (nl >= sizeof presented) nl = sizeof presented - 1;
            memcpy(presented, h, nl);
            presented[nl] = 0;
          }
        }
        if (!presented[0]) json_str(body, "current_token", presented, sizeof presented);
        if (!presented[0] || !cur[0] || strcmp(presented, cur) != 0) {
          resp(cfd, 401, "application/json",
               "{\"error\":\"peer_token already configured — send current token "
               "OR labauth master (action admin_reset, field master) to reset\"}");
          free(req); close(cfd); return;
        }
      } else if (has && admin_ok) {
        /* admin reset authorized via labauth master */
      }
      if (!strcmp(action, "generate") || !strcmp(action, "admin_reset") ||
          !strcmp(action, "reset") || !token[0]) {
        /* 32 hex bytes from /dev/urandom */
        unsigned char raw[16];
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, raw, sizeof raw) != (ssize_t)sizeof raw) {
          if (fd >= 0) close(fd);
          resp(cfd, 500, "application/json", "{\"error\":\"urandom failed\"}");
          free(req); close(cfd); return;
        }
        close(fd);
        static const char *hx = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
          token[i * 2] = hx[raw[i] >> 4];
          token[i * 2 + 1] = hx[raw[i] & 0xf];
        }
        token[32] = 0;
      }
      /* validate hex-ish token */
      size_t tl = strlen(token);
      if (tl < 16 || tl > 80) {
        resp(cfd, 400, "application/json", "{\"error\":\"token length 16..80\"}");
        free(req); close(cfd); return;
      }
      run_sh("mkdir -p /mnt/data/nanobot");
      char line[128];
      snprintf(line, sizeof line, "token=%s\n", token);
      write_small(pathf, line);
      chmod(pathf, 0600);
      char out[320];
      snprintf(out, sizeof out,
        "{\"ok\":true,\"action\":\"%s\",\"token\":\"%s\","
        "\"path\":\"/mnt/data/nanobot/peer_token\","
        "\"provider_seal\":\"peer_token KDF seals Grok OAuth at rest\","
        "\"note\":\"save token in ClankerDash / wrapper; shown once\"}",
        action, token);
      resp(cfd, 200, "application/json", out);
    } else {
      resp(cfd, 400, "application/json",
           "{\"error\":\"action generate|set|status\"}");
    }

  /* Optional SAM voice on robot speaker (nanobot-wrapper/speak — not nanobot core). */
  } else if (is_get && !strcmp(path, "/api/v1/assist/speak")) {
    int has_sam = (access("/mnt/data/nanobot-wrapper/speak/bin/sam", X_OK) == 0);
    int has_sh = (access("/mnt/data/nanobot-wrapper/speak/sam_speak.sh", X_OK) == 0);
    int has_vol = (access("/mnt/data/nanobot-wrapper/speak/bin/wav_vol", X_OK) == 0);
    int en = 0, volume = 35, pitch = 64, speed = 72, throat = 128, mouth = 128;
    FILE *ef = fopen("/mnt/data/nanobot/speak.env", "r");
    if (ef) {
      char line[256];
      while (fgets(line, sizeof line, ef)) {
        if (!strncmp(line, "SPEAK_ENABLED=", 14)) {
          en = (line[14] == '1' || !strncmp(line + 14, "on", 2) || !strncmp(line + 14, "true", 4));
        } else if (!strncmp(line, "SPEAK_VOLUME=", 13)) {
          volume = atoi(line + 13);
        } else if (!strncmp(line, "SAM_PITCH=", 10)) {
          pitch = atoi(line + 10);
        } else if (!strncmp(line, "SAM_SPEED=", 10)) {
          speed = atoi(line + 10);
        } else if (!strncmp(line, "SAM_THROAT=", 11)) {
          throat = atoi(line + 11);
        } else if (!strncmp(line, "SAM_MOUTH=", 10)) {
          mouth = atoi(line + 10);
        }
      }
      fclose(ef);
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    char out[420];
    snprintf(out, sizeof out,
      "{\"ok\":true,\"service\":\"sam-speak\",\"enabled\":%s,"
      "\"volume\":%d,\"pitch\":%d,\"speed\":%d,\"throat\":%d,\"mouth\":%d,"
      "\"sam\":%s,\"sam_speak\":%s,\"wav_vol\":%s,"
      "\"path\":\"/mnt/data/nanobot-wrapper/speak\","
      "\"note\":\"POST /api/v1/assist/speak {text,volume} plays on robot\"}",
      en ? "true" : "false", volume, pitch, speed, throat, mouth,
      has_sam ? "true" : "false", has_sh ? "true" : "false",
      has_vol ? "true" : "false");
    resp(cfd, 200, "application/json", out);

  } else if ((is_post || is_put) &&
             (!strcmp(path, "/api/v1/assist/speak") ||
              !strcmp(path, "/api/v1/assist/speak/test"))) {
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    int is_test = !strcmp(path, "/api/v1/assist/speak/test");
    char action[24] = {0};
    char text[1600] = {0};
    char phrase[220] = {0};
    json_str(body, "action", action, sizeof action);
    json_str(body, "text", text, sizeof text);
    if (!text[0]) json_str(body, "reply", text, sizeof text);
    json_str(body, "test_phrase", phrase, sizeof phrase);
    if (is_test || !strcmp(action, "test")) {
      is_test = 1;
      if (!text[0]) {
        if (phrase[0])
          snprintf(text, sizeof text, "%s", phrase);
        else
          snprintf(text, sizeof text, "Hello. Soft voice on Clanker.");
      }
    }
    /* config save */
    if (!strcmp(action, "config") || (!is_test && !text[0] && is_put)) {
      int en = 0, volume = 35, pitch = 64, speed = 72, throat = 128, mouth = 128;
      {
        const char *e = strstr(body, "\"enabled\"");
        if (e) {
          e = strchr(e, ':');
          if (e) {
            while (*e && (*e == ':' || *e == ' ' || *e == '\t')) e++;
            if (!strncmp(e, "true", 4) || e[0] == '1') en = 1;
          }
        }
      }
      json_int(body, "volume", &volume);
      json_int(body, "pitch", &pitch);
      json_int(body, "speed", &speed);
      json_int(body, "throat", &throat);
      json_int(body, "mouth", &mouth);
      if (volume < 0) volume = 0;
      if (volume > 100) volume = 100;
      run_sh("mkdir -p /mnt/data/nanobot");
      {
        FILE *ef = fopen("/mnt/data/nanobot/speak.env", "w");
        if (!ef) {
          resp(cfd, 500, "application/json", "{\"error\":\"cannot write speak.env\"}");
          free(req); close(cfd); return;
        }
        fprintf(ef,
                "SPEAK_ENABLED=%d\nSPEAK_VOLUME=%d\n"
                "SAM_PITCH=%d\nSAM_SPEED=%d\nSAM_THROAT=%d\nSAM_MOUTH=%d\n"
                "SPEAK_MAX_CHARS=800\nSPEAK_AUTO_REPLY=1\n"
                "SPEAK_TEST_PHRASE=\"Hello. Soft voice on Clanker.\"\n"
                "SAM_BIN=/mnt/data/nanobot-wrapper/speak/bin/sam\n"
                "APLAY_BIN=/mnt/data/audio-bin/bin/aplay\n"
                "WAV_VOL_BIN=/mnt/data/nanobot-wrapper/speak/bin/wav_vol\n"
                "SPEAK_TMPDIR=/dev/shm\n",
                en, volume, pitch, speed, throat, mouth);
        fclose(ef);
      }
      char out[200];
      snprintf(out, sizeof out,
               "{\"ok\":true,\"saved\":true,\"enabled\":%s,\"volume\":%d,"
               "\"pitch\":%d,\"speed\":%d,\"throat\":%d,\"mouth\":%d}",
               en ? "true" : "false", volume, pitch, speed, throat, mouth);
      resp(cfd, 200, "application/json", out);
      free(req); close(cfd); return;
    }
    if (!text[0]) {
      resp(cfd, 400, "application/json",
           "{\"error\":\"need text (or POST …/speak/test)\"}");
      free(req); close(cfd); return;
    }
    if (access("/mnt/data/nanobot-wrapper/speak/sam_speak.sh", X_OK) != 0 ||
        access("/mnt/data/nanobot-wrapper/speak/bin/sam", X_OK) != 0) {
      resp(cfd, 503, "application/json",
           "{\"error\":\"SAM not installed on robot "
           "(need /mnt/data/nanobot-wrapper/speak)\"}");
      free(req); close(cfd); return;
    }
    /* Defaults from saved speak.env, then JSON body always wins when present
     * (browser Play/Test / stream must override robot config without Save). */
    int volume = 35, pitch = 64, speed = 72, throat = 128, mouth = 128;
    int has_volume = 0, has_pitch = 0, has_speed = 0, has_throat = 0, has_mouth = 0;
    {
      FILE *ef = fopen("/mnt/data/nanobot/speak.env", "r");
      if (ef) {
        char line[256];
        while (fgets(line, sizeof line, ef)) {
          if (!strncmp(line, "SPEAK_VOLUME=", 13)) volume = atoi(line + 13);
          else if (!strncmp(line, "SAM_PITCH=", 10)) pitch = atoi(line + 10);
          else if (!strncmp(line, "SAM_SPEED=", 10)) speed = atoi(line + 10);
          else if (!strncmp(line, "SAM_THROAT=", 11)) throat = atoi(line + 11);
          else if (!strncmp(line, "SAM_MOUTH=", 10)) mouth = atoi(line + 10);
        }
        fclose(ef);
      }
    }
    has_volume = json_int(body, "volume", &volume);
    has_pitch = json_int(body, "pitch", &pitch);
    has_speed = json_int(body, "speed", &speed);
    has_throat = json_int(body, "throat", &throat);
    has_mouth = json_int(body, "mouth", &mouth);
    (void)has_volume; (void)has_pitch; (void)has_speed;
    (void)has_throat; (void)has_mouth;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    /* Force CLI flags so sam_speak.sh does not re-apply speak.env over body */
    /* (sam_speak sources SPEAK_ENV first, then --volume/--pitch CLI wins) */
    /* wait=true (default): block until spoken — needed for stream phrase queue order.
     * wait=false / async=true: fire-and-forget (legacy full-reply speak). */
    int wait_done = 1;
    {
      const char *w = strstr(body, "\"wait\"");
      if (w) {
        w = strchr(w, ':');
        if (w) {
          while (*w && (*w == ':' || *w == ' ' || *w == '\t')) w++;
          if (!strncmp(w, "false", 5) || w[0] == '0') wait_done = 0;
        }
      }
      const char *a = strstr(body, "\"async\"");
      if (a) {
        a = strchr(a, ':');
        if (a) {
          while (*a && (*a == ':' || *a == ' ' || *a == '\t')) a++;
          if (!strncmp(a, "true", 4) || a[0] == '1') wait_done = 0;
        }
      }
    }
    if (is_test) wait_done = 1;
    /* Unique stdin file so concurrent stream phrases do not race /dev/shm/speak.in */
    char inpath[72];
    snprintf(inpath, sizeof inpath, "/dev/shm/speak.%u.%ld.in",
             (unsigned)getpid(), (long)time(NULL));
    {
      FILE *tf = fopen(inpath, "w");
      if (!tf) {
        resp(cfd, 500, "application/json", "{\"error\":\"cannot write speak temp\"}");
        free(req); close(cfd); return;
      }
      fputs(text, tf);
      fclose(tf);
    }
    {
      char cmd[1024];
      if (wait_done) {
        snprintf(cmd, sizeof cmd,
                 "HOME=/mnt/data/rockctl "
                 "SPEAK_ENV=/mnt/data/nanobot/speak.env "
                 "SPEAK_HOME=/mnt/data/nanobot-wrapper/speak "
                 "SPEAK_ALSA_DEVICE=clanker "
                 "PATH=/mnt/data/nanobot-wrapper/speak/bin:/mnt/data/audio-bin/bin:/usr/bin:/bin "
                 "LD_LIBRARY_PATH=/mnt/data/audio-bin/lib "
                 "SAM_BIN=/mnt/data/nanobot-wrapper/speak/bin/sam "
                 "APLAY_BIN=/mnt/data/audio-bin/bin/aplay "
                 "WAV_VOL_BIN=/mnt/data/nanobot-wrapper/speak/bin/wav_vol "
                 "SPEAK_TMPDIR=/dev/shm "
                 "/mnt/data/nanobot-wrapper/speak/sam_speak.sh --force "
                 "--volume %d --pitch %d --speed %d --throat %d --mouth %d "
                 "<%s >>/mnt/data/nanobot-wrapper/speak/speak.log 2>&1; "
                 "rm -f %s",
                 volume, pitch, speed, throat, mouth, inpath, inpath);
        int rc = run_sh(cmd);
        if (rc != 0 && is_test) {
          char err[160];
          snprintf(err, sizeof err,
                   "{\"ok\":false,\"error\":\"sam_speak exit %d — see "
                   "/mnt/data/nanobot-wrapper/speak/speak.log\"}",
                   rc);
          resp(cfd, 500, "application/json", err);
          free(req); close(cfd); return;
        }
        resp(cfd, 200, "application/json",
             "{\"ok\":true,\"played\":true,\"where\":\"robot\","
             "\"note\":\"spoken on robot speaker\"}");
      } else {
        /* async: unique file cleaned after speak finishes in subshell */
        snprintf(cmd, sizeof cmd,
                 "( HOME=/mnt/data/rockctl "
                 "SPEAK_ENV=/mnt/data/nanobot/speak.env "
                 "SPEAK_HOME=/mnt/data/nanobot-wrapper/speak "
                 "SPEAK_ALSA_DEVICE=clanker "
                 "PATH=/mnt/data/nanobot-wrapper/speak/bin:/mnt/data/audio-bin/bin:/usr/bin:/bin "
                 "LD_LIBRARY_PATH=/mnt/data/audio-bin/lib "
                 "SAM_BIN=/mnt/data/nanobot-wrapper/speak/bin/sam "
                 "APLAY_BIN=/mnt/data/audio-bin/bin/aplay "
                 "WAV_VOL_BIN=/mnt/data/nanobot-wrapper/speak/bin/wav_vol "
                 "SPEAK_TMPDIR=/dev/shm "
                 "/mnt/data/nanobot-wrapper/speak/sam_speak.sh --force "
                 "--volume %d --pitch %d --speed %d --throat %d --mouth %d "
                 "<%s >>/mnt/data/nanobot-wrapper/speak/speak.log 2>&1; "
                 "rm -f %s ) &",
                 volume, pitch, speed, throat, mouth, inpath, inpath);
        run_sh(cmd);
        resp(cfd, 200, "application/json",
             "{\"ok\":true,\"playing\":true,\"where\":\"robot\","
             "\"note\":\"queued on robot speaker\"}");
      }
    }

  } else if ((is_post || is_put) && !strcmp(path, "/api/v1/services")) {
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    char service[32] = {0}, action[24] = {0};
    json_str(body, "service", service, sizeof service);
    if (!service[0]) json_str(body, "name", service, sizeof service);
    json_str(body, "action", action, sizeof action);
    /* also accept enabled true/false */
    int en = -1;
    {
      const char *e = strstr(body, "\"enabled\"");
      if (e) {
        e = strchr(e, ':');
        if (e) {
          while (*e && (*e == ':' || *e == ' ' || *e == '\t')) e++;
          if (!strncmp(e, "true", 4) || e[0] == '1') en = 1;
          else if (!strncmp(e, "false", 5) || e[0] == '0') en = 0;
        }
      }
    }
    if (!action[0]) {
      if (en == 1) snprintf(action, sizeof action, "on");
      else if (en == 0) snprintf(action, sizeof action, "off");
    }
    if (!service[0] || !action[0]) {
      resp(cfd, 400, "application/json",
           "{\"error\":\"need service (assist|ui|shell|watcher) and action on|off or enabled\"}");
    } else if (!strcmp(service, "assist") || !strcmp(service, "nanobot") || !strcmp(service, "assistant")) {
      if (!strcmp(action, "on") || !strcmp(action, "start") || !strcmp(action, "enable")) {
        /* Prefer run.sh so /mnt/data/nanobot/settings (UI/shell/www) apply */
        run_sh("mkdir -p /mnt/data/nanobot; export NANOBOT_HOME=/mnt/data/nanobot; "
               "if ! kill -0 $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; then "
               "  if [ -x /mnt/data/nanobot/run.sh ]; then "
               "    /mnt/data/nanobot/run.sh >>/mnt/data/nanobot/nanobot.out 2>&1 & "
               "    echo $! >/mnt/data/nanobot/nanobot.pid; "
               "  elif [ -x /mnt/data/nanobot/bin/nanobot ]; then "
               "    /mnt/data/nanobot/bin/nanobot --home /mnt/data/nanobot --port 8787 "
               "      >>/mnt/data/nanobot/nanobot.out 2>&1 & echo $! >/mnt/data/nanobot/nanobot.pid; "
               "  fi; "
               "fi");
        resp(cfd, 200, "application/json", "{\"ok\":true,\"service\":\"assist\",\"action\":\"on\"}");
      } else if (!strcmp(action, "off") || !strcmp(action, "stop") || !strcmp(action, "disable")) {
        run_sh("kill $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; "
               "killall nanobot 2>/dev/null; true");
        resp(cfd, 200, "application/json", "{\"ok\":true,\"service\":\"assist\",\"action\":\"off\"}");
      } else {
        resp(cfd, 400, "application/json", "{\"error\":\"action on|off\"}");
      }
    } else if (!strcmp(service, "ui") || !strcmp(service, "www") || !strcmp(service, "web")) {
      /* Persist nanobot Web UI preference (run.sh applies on next assist start). */
      if (!strcmp(action, "on") || !strcmp(action, "enable") || !strcmp(action, "start")) {
        nanobot_settings_set("UI", "on");
        {
          char www[256];
          nanobot_settings_get("WWW", www, sizeof www, "");
          if (!www[0])
            nanobot_settings_set("WWW", "/mnt/data/nanobot-wrapper/www");
        }
        /* Restart assist so --www takes effect when turning on. */
        run_sh("kill $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; "
               "killall nanobot 2>/dev/null; true; "
               "sleep 1; "
               "export NANOBOT_HOME=/mnt/data/nanobot; "
               "if [ -x /mnt/data/nanobot/run.sh ]; then "
               "  /mnt/data/nanobot/run.sh >>/mnt/data/nanobot/nanobot.out 2>&1 & "
               "  echo $! >/mnt/data/nanobot/nanobot.pid; "
               "fi");
        resp(cfd, 200, "application/json",
             "{\"ok\":true,\"service\":\"ui\",\"enabled\":true,\"note\":\"assist restarted with Web UI\"}");
      } else if (!strcmp(action, "off") || !strcmp(action, "disable") || !strcmp(action, "stop")) {
        nanobot_settings_set("UI", "off");
        run_sh("kill $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; "
               "killall nanobot 2>/dev/null; true; "
               "sleep 1; "
               "export NANOBOT_HOME=/mnt/data/nanobot; "
               "if [ -x /mnt/data/nanobot/run.sh ]; then "
               "  /mnt/data/nanobot/run.sh >>/mnt/data/nanobot/nanobot.out 2>&1 & "
               "  echo $! >/mnt/data/nanobot/nanobot.pid; "
               "fi");
        resp(cfd, 200, "application/json",
             "{\"ok\":true,\"service\":\"ui\",\"enabled\":false,\"note\":\"assist restarted without Web UI\"}");
      } else {
        resp(cfd, 400, "application/json", "{\"error\":\"action on|off\"}");
      }
    } else if (!strcmp(service, "shell")) {
      if (!strcmp(action, "on") || !strcmp(action, "enable")) {
        write_small("/mnt/data/nanobot/shell_enabled", "1");
        nanobot_settings_set("SHELL", "on");
        resp(cfd, 200, "application/json", "{\"ok\":true,\"service\":\"shell\",\"enabled\":true}");
      } else if (!strcmp(action, "off") || !strcmp(action, "disable")) {
        write_small("/mnt/data/nanobot/shell_enabled", "0");
        nanobot_settings_set("SHELL", "off");
        resp(cfd, 200, "application/json", "{\"ok\":true,\"service\":\"shell\",\"enabled\":false}");
      } else {
        resp(cfd, 400, "application/json", "{\"error\":\"action on|off\"}");
      }
    } else if (!strcmp(service, "watcher")) {
      if (!strcmp(action, "on") || !strcmp(action, "enable")) {
        write_small("/mnt/data/nanobot/watcher_enabled", "1");
        run_sh("mkdir -p /mnt/data/nanobot/jobs; "
               "echo 'watcher on' >> /mnt/data/nanobot/watcher.log");
        resp(cfd, 200, "application/json", "{\"ok\":true,\"service\":\"watcher\",\"enabled\":true}");
      } else if (!strcmp(action, "off") || !strcmp(action, "disable")) {
        write_small("/mnt/data/nanobot/watcher_enabled", "0");
        resp(cfd, 200, "application/json", "{\"ok\":true,\"service\":\"watcher\",\"enabled\":false}");
      } else {
        resp(cfd, 400, "application/json", "{\"error\":\"action on|off\"}");
      }
    } else if (!strcmp(service, "rockctl")) {
      /* cannot stop self cleanly from handler; report only */
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"service\":\"rockctl\",\"note\":\"always online while this API responds; use POST /api/v1/system action=restart_stack\"}");
    } else if (!strcmp(service, "stack") || !strcmp(service, "all")) {
      /* alias → full stack restart (same as /api/v1/system) */
      if (!strcmp(action, "restart") || !strcmp(action, "on") || !strcmp(action, "reload")) {
        run_sh("nohup /bin/sh -c '"
               "sleep 1; "
               "killall -9 lhlam 2>/dev/null; true; "
               "kill $(cat /mnt/data/lhlam/session.pid 2>/dev/null) 2>/dev/null; true; "
               "kill $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; "
               "killall nanobot 2>/dev/null; true; "
               "sleep 1; "
               "export NANOBOT_HOME=/mnt/data/nanobot; "
               "if [ -x /mnt/data/nanobot/run.sh ]; then "
               "  /mnt/data/nanobot/run.sh >>/mnt/data/nanobot/nanobot.out 2>&1 & "
               "  echo $! >/mnt/data/nanobot/nanobot.pid; "
               "fi; "
               "if [ -f /mnt/data/lhlam/session.flag ] && [ -x /mnt/data/lhlam/scripts/session_loop.sh ]; then "
               "  cd /mnt/data/lhlam && nohup ./scripts/session_loop.sh >>lab/logs/session_loop.log 2>&1 & "
               "  echo $! >/mnt/data/lhlam/session.pid; "
               "fi; "
               "killall rockctl 2>/dev/null; true; "
               "' >/mnt/data/rockctl/system_restart.log 2>&1 &");
        resp(cfd, 200, "application/json",
             "{\"ok\":true,\"service\":\"stack\",\"action\":\"restart\","
             "\"note\":\"assist+train restarting; rockctl will respawn via watchdog in ~few seconds\"}");
      } else {
        resp(cfd, 400, "application/json", "{\"error\":\"stack action restart\"}");
      }
    } else {
      resp(cfd, 400, "application/json", "{\"error\":\"unknown service\"}");
    }

  } else if ((is_post || is_put) && !strcmp(path, "/api/v1/system")) {
    /* Settings → Restart stack / Reboot robot. Deferred so HTTP can answer first. */
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char action[32] = {0};
    json_str(body, "action", action, sizeof action);
    if (!action[0]) json_str(body, "cmd", action, sizeof action);
    if (!action[0]) {
      resp(cfd, 400, "application/json",
           "{\"error\":\"need action: restart_stack|restart_assist|restart_train|reboot\"}");
    } else if (!strcmp(action, "restart_stack") || !strcmp(action, "restart") ||
               !strcmp(action, "reload")) {
      /* Halt RC, bounce assist + train, then rockctl (watchdog brings it back). */
      run_sh("nohup /bin/sh -c '"
             "echo \"$(date) restart_stack\" >>/mnt/data/rockctl/system_restart.log; "
             "wget -q -T 2 -O- --post-data=\"{\\\"action\\\":\\\"halt\\\",\\\"async\\\":true}\" "
             "  --header=\"Content-Type: application/json\" "
             "  http://127.0.0.1:8080/api/v1/manual/async 2>/dev/null; true; "
             "sleep 1; "
             "killall -9 lhlam 2>/dev/null; true; "
             "kill $(cat /mnt/data/lhlam/session.pid 2>/dev/null) 2>/dev/null; true; "
             "kill $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; "
             "killall nanobot 2>/dev/null; true; "
             "sleep 1; "
             "export NANOBOT_HOME=/mnt/data/nanobot; "
             "if [ -x /mnt/data/nanobot/run.sh ]; then "
             "  /mnt/data/nanobot/run.sh >>/mnt/data/nanobot/nanobot.out 2>&1 & "
             "  echo $! >/mnt/data/nanobot/nanobot.pid; "
             "elif [ -x /mnt/data/nanobot/bin/nanobot ]; then "
             "  /mnt/data/nanobot/bin/nanobot --home /mnt/data/nanobot --port 8787 "
             "    >>/mnt/data/nanobot/nanobot.out 2>&1 & echo $! >/mnt/data/nanobot/nanobot.pid; "
             "fi; "
             "if [ -f /mnt/data/lhlam/session.flag ] && [ -x /mnt/data/lhlam/scripts/session_loop.sh ]; then "
             "  cd /mnt/data/lhlam && nohup ./scripts/session_loop.sh >>lab/logs/session_loop.log 2>&1 & "
             "  echo $! >/mnt/data/lhlam/session.pid; "
             "fi; "
             "sleep 1; "
             "killall rockctl 2>/dev/null; true; "
             "' >/mnt/data/rockctl/system_restart.log 2>&1 &");
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"action\":\"restart_stack\","
           "\"note\":\"halting RC, restarting assist+train; rockctl restarts via watchdog (~5–15s). Refresh dash after.\"}");
    } else if (!strcmp(action, "restart_assist") || !strcmp(action, "restart_nanobot")) {
      run_sh("nohup /bin/sh -c '"
             "kill $(cat /mnt/data/nanobot/nanobot.pid 2>/dev/null) 2>/dev/null; "
             "killall nanobot 2>/dev/null; true; sleep 1; "
             "export NANOBOT_HOME=/mnt/data/nanobot; "
             "if [ -x /mnt/data/nanobot/run.sh ]; then "
             "  /mnt/data/nanobot/run.sh >>/mnt/data/nanobot/nanobot.out 2>&1 & "
             "  echo $! >/mnt/data/nanobot/nanobot.pid; "
             "fi"
             "' >/mnt/data/rockctl/system_restart.log 2>&1 &");
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"action\":\"restart_assist\",\"note\":\"assist peer restarting\"}");
    } else if (!strcmp(action, "restart_train") || !strcmp(action, "restart_session")) {
      run_sh("nohup /bin/sh -c '"
             "killall -9 lhlam 2>/dev/null; true; "
             "kill $(cat /mnt/data/lhlam/session.pid 2>/dev/null) 2>/dev/null; true; "
             "sleep 1; "
             "touch /mnt/data/lhlam/session.flag; "
             "if [ -x /mnt/data/lhlam/scripts/session_loop.sh ]; then "
             "  cd /mnt/data/lhlam && nohup ./scripts/session_loop.sh >>lab/logs/session_loop.log 2>&1 & "
             "  echo $! >/mnt/data/lhlam/session.pid; "
             "fi"
             "' >/mnt/data/rockctl/system_restart.log 2>&1 &");
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"action\":\"restart_train\",\"note\":\"lhlam session_loop restarting\"}");
    } else if (!strcmp(action, "reboot") || !strcmp(action, "reboot_robot")) {
      run_sh("nohup /bin/sh -c '"
             "echo \"$(date) reboot\" >>/mnt/data/rockctl/system_restart.log; "
             "wget -q -T 2 -O- --post-data=\"{\\\"action\\\":\\\"halt\\\",\\\"async\\\":true}\" "
             "  --header=\"Content-Type: application/json\" "
             "  http://127.0.0.1:8080/api/v1/manual/async 2>/dev/null; true; "
             "sync; sleep 2; reboot"
             "' >/mnt/data/rockctl/system_restart.log 2>&1 &");
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"action\":\"reboot\","
           "\"note\":\"robot rebooting in ~2s — RC halted, sync done. Dash will drop until boot.\"}");
    } else {
      resp(cfd, 400, "application/json",
           "{\"error\":\"unknown action\",\"want\":\"restart_stack|restart_assist|restart_train|reboot\"}");
    }

  } else if (is_get && !strcmp(path, "/api/v1/system")) {
    resp(cfd, 200, "application/json",
         "{\"ok\":true,\"actions\":[\"restart_stack\",\"restart_assist\",\"restart_train\",\"reboot\"],"
         "\"note\":\"POST /api/v1/system {\\\"action\\\":\\\"restart_stack\\\"}\"}");

  } else if (is_get && (!strcmp(path, "/api/v1/train/status") || !strcmp(path, "/api/v1/train"))) {
    /* Cheap file reads only — no miio. Used by Dash Train tab. */
    char report[512] = {0}, sess[512] = {0};
    int running = 0;
    char mode[32] = "stopped";
    {
      FILE *f = fopen("/mnt/data/nanobot/braincube/last_report.txt", "r");
      if (f) {
        size_t n = fread(report, 1, sizeof report - 1, f);
        report[n] = 0;
        fclose(f);
      }
    }
    {
      FILE *f = fopen("/mnt/data/nanobot/braincube/session_status.json", "r");
      if (f) {
        size_t n = fread(sess, 1, sizeof sess - 1, f);
        sess[n] = 0;
        fclose(f);
      }
    }
    if (access("/mnt/data/lhlam/session.flag", F_OK) == 0) running = 1;
    if (strstr(sess, "\"running\":true") || strstr(sess, "\"running\": true"))
      running = 1;
    if (strstr(sess, "lhlam_observe") || strstr(sess, "observe") || strstr(sess, "clean"))
      snprintf(mode, sizeof mode, "observe");
    else if (strstr(sess, "lhlam_teach") || strstr(sess, "rc") || strstr(sess, "teach"))
      snprintf(mode, sizeof mode, "rc_teach");
    else if (running)
      snprintf(mode, sizeof mode, "session");
    /* sanitize report for JSON string */
    {
      char rep_esc[640];
      size_t i, o = 0;
      for (i = 0; report[i] && o + 2 < sizeof rep_esc; i++) {
        char c = report[i];
        if (c == '"' || c == '\\') {
          rep_esc[o++] = '\\';
          rep_esc[o++] = c;
        } else if (c == '\n' || c == '\r') {
          rep_esc[o++] = ' ';
        } else if ((unsigned char)c < 32) {
          continue;
        } else {
          rep_esc[o++] = c;
        }
      }
      rep_esc[o] = 0;
      /* session blob: only include if looks like JSON object */
      char body[1600];
      if (sess[0] == '{') {
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"running\":%s,\"mode\":\"%s\",\"report\":\"%s\",\"session\":%s}",
                 running ? "true" : "false", mode, rep_esc, sess);
      } else {
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"running\":%s,\"mode\":\"%s\",\"report\":\"%s\"}",
                 running ? "true" : "false", mode, rep_esc);
      }
      resp(cfd, 200, "application/json", body);
    }

  } else if (is_get && !strcmp(path, "/api/v1/wifi")) {
    char ip[48] = {0};
    int up = wifi_sta_up(ip, sizeof ip);
    char ssid[64] = {0};
    {
      FILE *f = fopen("/mnt/data/miio/wifi.conf", "r");
      if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
          if (!strncmp(line, "ssid=", 5)) {
            char *s = line + 5;
            while (*s == '"' || *s == ' ') s++;
            size_t i = 0;
            while (*s && *s != '"' && *s != '\n' && i + 1 < sizeof ssid) ssid[i++] = *s++;
            ssid[i] = 0;
            break;
          }
        }
        fclose(f);
      }
    }
    char body[320];
    snprintf(body, sizeof body,
      "{\"ok\":true,\"sta\":%s,\"ip\":\"%s\",\"ssid\":\"%s\","
      "\"note\":\"off drops LAN; recover via USB ADB/SSH on cable\"}",
      up ? "true" : "false", ip, ssid);
    resp(cfd, 200, "application/json", body);

  } else if ((is_post || is_put) && !strcmp(path, "/api/v1/wifi")) {
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    char action[24] = {0};
    json_str(body, "action", action, sizeof action);
    if (!action[0]) json_str(body, "state", action, sizeof action);
    if (!strcmp(action, "off") || !strcmp(action, "down") || !strcmp(action, "disable")) {
      /* STA down — keep adbd/SSH over USB for recovery */
      run_sh("ip link set wlan0 down 2>/dev/null; true");
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"wifi\":\"off\",\"recover\":\"USB ADB or dock ethernet path; wifi on via USB shell\"}");
    } else if (!strcmp(action, "on") || !strcmp(action, "up") || !strcmp(action, "enable")) {
      run_sh("ip link set wlan0 up 2>/dev/null; "
             "if [ -x /opt/rockrobo/wlan/wifi_start.sh ]; then "
             "  /opt/rockrobo/wlan/wifi_start.sh >>/mnt/data/rockctl/wifi.log 2>&1 & "
             "fi; true");
      resp(cfd, 200, "application/json", "{\"ok\":true,\"wifi\":\"on\"}");
    } else if (!strcmp(action, "status")) {
      char ip[48] = {0};
      int up = wifi_sta_up(ip, sizeof ip);
      char b[160];
      snprintf(b, sizeof b, "{\"ok\":true,\"sta\":%s,\"ip\":\"%s\"}", up ? "true" : "false", ip);
      resp(cfd, 200, "application/json", b);
    } else {
      resp(cfd, 400, "application/json", "{\"error\":\"action on|off|status\"}");
    }

  } else if (is_put && !strcmp(path, "/api/v1/water")) {
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    char level[24];
    int want_sync = body && (strstr(body, "\"sync\":true") || strstr(body, "\"sync\":1"));
    if (!json_str(body, "level", level, sizeof level)) {
      resp(cfd, 400, "application/json", "{\"error\":\"missing level off|low|medium|high\"}");
    } else if (water_mode(level) < 0) {
      resp(cfd, 400, "application/json", "{\"error\":\"bad level\"}");
    } else if (!want_sync) {
      spawn_async_act(m, "water", level);
      resp(cfd, 202, "application/json", "{\"ok\":true,\"async\":true,\"water\":true}");
    } else {
      int mode = water_mode(level);
      char params[32];
      snprintf(params, sizeof params, "[%d]", mode);
      char *r = NULL;
      if (miio_call(m, "set_water_box_custom_mode", params, &r) != 0)
        resp(cfd, 502, "application/json", "{\"error\":\"miio water failed\"}");
      else { resp(cfd, 200, "application/json", r ? r : "{\"result\":[\"ok\"]}"); free(r); }
    }

  } else if (is_get && !strcmp(path, "/api/v1/schedule")) {
    FILE *f = fopen("/mnt/data/rockctl/schedule.json", "r");
    if (!f) {
      resp(cfd, 200, "application/json", "{\"ok\":true,\"jobs\":[]}");
    } else {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      buf[n] = 0;
      if (!n) resp(cfd, 200, "application/json", "{\"ok\":true,\"jobs\":[]}");
      else resp(cfd, 200, "application/json", buf);
    }

  } else if ((is_post || is_put) && !strcmp(path, "/api/v1/schedule")) {
    /* Full list: {"ok":true,"jobs":[...]}  or single job fields (becomes one-job list).
     * ClankerDash always PUTs the full jobs array so edits/deletes are reliable. */
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    while (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r') body++;
    if (body[0] != '{') {
      resp(cfd, 400, "application/json", "{\"error\":\"JSON object required\"}");
    } else {
      char store[4096];
      if (strstr(body, "\"jobs\"")) {
        if (strstr(body, "\"ok\""))
          snprintf(store, sizeof store, "%s", body);
        else {
          const char *jp = strstr(body, "\"jobs\"");
          snprintf(store, sizeof store, "{\"ok\":true,%s", jp ? jp : "\"jobs\":[]}");
        }
      } else {
        char id[48] = {0}, fan[24] = {0}, water[24] = {0}, type[16] = {0}, dow[32] = {0};
        int hh = 9, mm = 0, cycles = 1, enabled = 1;
        json_str(body, "id", id, sizeof id);
        json_str(body, "fan", fan, sizeof fan);
        json_str(body, "water", water, sizeof water);
        json_str(body, "type", type, sizeof type);
        json_str(body, "dow", dow, sizeof dow);
        {
          const char *h = strstr(body, "\"hh\"");
          if (h) { h = strchr(h, ':'); if (h) hh = atoi(h + 1); }
        }
        {
          const char *h = strstr(body, "\"mm\"");
          if (h) { h = strchr(h, ':'); if (h) mm = atoi(h + 1); }
        }
        {
          const char *h = strstr(body, "\"cycles\"");
          if (h) { h = strchr(h, ':'); if (h) cycles = atoi(h + 1); }
        }
        {
          const char *e = strstr(body, "\"enabled\"");
          if (e) {
            e = strchr(e, ':');
            if (e) {
              while (*e && (*e == ':' || *e == ' ')) e++;
              if (!strncmp(e, "false", 5) || e[0] == '0') enabled = 0;
            }
          }
        }
        if (!id[0]) snprintf(id, sizeof id, "job%d", (int)(time(NULL) % 100000));
        if (!fan[0]) snprintf(fan, sizeof fan, "balanced");
        if (!water[0]) snprintf(water, sizeof water, "off");
        if (!type[0]) snprintf(type, sizeof type, "auto");
        if (cycles < 1) cycles = 1;
        if (cycles > 3) cycles = 3;
        if (hh < 0) hh = 0;
        if (hh > 23) hh = 23;
        if (mm < 0) mm = 0;
        if (mm > 59) mm = 59;
        snprintf(store, sizeof store,
          "{\"ok\":true,\"jobs\":[{\"id\":\"%s\",\"enabled\":%s,"
          "\"hh\":%d,\"mm\":%d,\"dow\":\"%s\",\"type\":\"%s\","
          "\"cycles\":%d,\"fan\":\"%s\",\"water\":\"%s\"}]}",
          id, enabled ? "true" : "false", hh, mm, dow, type, cycles, fan, water);
      }
      write_small("/mnt/data/rockctl/schedule.json", store);
      resp(cfd, 200, "application/json", store);
    }

  } else if (is_del && !strcmp(path, "/api/v1/schedule")) {
    unlink("/mnt/data/rockctl/schedule.json");
    unlink("/mnt/data/rockctl/schedule.last");
    resp(cfd, 200, "application/json", "{\"ok\":true,\"jobs\":[]}");

  } else if (is_post && !strcmp(path, "/api/v1/clean")) {
    /* one-shot clean with settings: type, cycles, fan, water */
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    char fan[24] = {0}, water[24] = {0}, type[16] = {0};
    int cycles = 1;
    json_str(body, "fan", fan, sizeof fan);
    json_str(body, "water", water, sizeof water);
    json_str(body, "type", type, sizeof type);
    json_str(body, "power", fan, sizeof fan); /* alias */
    json_str(body, "moist", water, sizeof water);
    {
      const char *h = strstr(body, "\"cycles\"");
      if (h) { h = strchr(h, ':'); if (h) cycles = atoi(h + 1); }
    }
    if (!type[0]) snprintf(type, sizeof type, "auto");
    if (!fan[0]) snprintf(fan, sizeof fan, "balanced");
    if (!water[0]) snprintf(water, sizeof water, "off");
    if (apply_clean_settings(m, fan, water, cycles, type) != 0)
      resp(cfd, 502, "application/json", "{\"error\":\"clean start failed\"}");
    else {
      char b[256];
      snprintf(b, sizeof b,
        "{\"ok\":true,\"type\":\"%s\",\"cycles\":%d,\"fan\":\"%s\",\"water\":\"%s\"}",
        type, cycles, fan, water);
      resp(cfd, 200, "application/json", b);
    }

  /* ----- Event sounds library + per-event config (ClankerDash Sounds) ----- */
  } else if (is_get && !strcmp(path, "/api/v1/sounds")) {
    sounds_ensure_dirs();
    /* list wav files in sounds/ */
    char files[2048]; size_t fo = 0; files[0] = 0;
    fo += (size_t)snprintf(files + fo, sizeof files - fo, "[");
    int ffirst = 1;
    DIR *d = opendir(SOUNDS_DIR);
    if (d) {
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 5) continue;
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcasecmp(dot, ".wav") != 0) continue;
        /* skip nested dirs named oddly */
        char pathm[320];
        snprintf(pathm, sizeof pathm, "%s/%s", SOUNDS_DIR, de->d_name);
        struct stat st;
        if (stat(pathm, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (fo + 96 >= sizeof files) break;
        fo += (size_t)snprintf(files + fo, sizeof files - fo,
          "%s{\"name\":\"%s\",\"bytes\":%ld}", ffirst ? "" : ",", de->d_name, (long)st.st_size);
        ffirst = 0;
      }
      closedir(d);
    }
    fo += (size_t)snprintf(files + fo, sizeof files - fo, "]");

    /* events */
    char events[4096]; size_t eo = 0; events[0] = 0;
    eo += (size_t)snprintf(events + eo, sizeof events - eo, "[");
    int efirst = 1;
    for (int i = 0; SOUND_EVENTS[i].id; i++) {
      const char *id = SOUND_EVENTS[i].id;
      char en[16], file[96], tts_text[200], vol[16], pitch[16], speed[16], throat[16], mouth[16];
      sounds_read_evt(id, "enabled", en, sizeof en, "1");
      sounds_read_evt(id, "file", file, sizeof file, SOUND_EVENTS[i].def_file);
      sounds_read_evt(id, "tts_text", tts_text, sizeof tts_text, "");
      sounds_read_evt(id, "tts_volume", vol, sizeof vol, "");
      sounds_read_evt(id, "tts_pitch", pitch, sizeof pitch, "");
      sounds_read_evt(id, "tts_speed", speed, sizeof speed, "");
      sounds_read_evt(id, "tts_throat", throat, sizeof throat, "");
      sounds_read_evt(id, "tts_mouth", mouth, sizeof mouth, "");
      /* legacy ouch_enabled overrides if present for ouch */
      if (!strcmp(id, "ouch")) {
        char leg[16];
        read_small("/mnt/data/rockctl/ouch_enabled", leg, sizeof leg, "");
        if (leg[0]) snprintf(en, sizeof en, "%s", leg);
      }
      int enabled = 1;
      if (en[0] == '0' || !strcasecmp(en, "off") || !strcasecmp(en, "false") || !strcasecmp(en, "no"))
        enabled = 0;
      char full[320] = "";
      long bytes = 0;
      int has_file = 0;
      if (file[0]) {
        snprintf(full, sizeof full, "%s/%s", SOUNDS_DIR, file);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
          has_file = 1;
          bytes = (long)st.st_size;
        }
      }
      /* escape tts_text for JSON (minimal) */
      char tts_esc[220]; size_t ti = 0;
      for (const char *p = tts_text; *p && ti + 2 < sizeof tts_esc; p++) {
        if (*p == '"' || *p == '\\') tts_esc[ti++] = '\\';
        if ((unsigned char)*p >= 32) tts_esc[ti++] = *p;
      }
      tts_esc[ti] = 0;
      if (eo + 400 >= sizeof events) break;
      eo += (size_t)snprintf(events + eo, sizeof events - eo,
        "%s{\"id\":\"%s\",\"label\":\"%s\",\"enabled\":%s,"
        "\"file\":\"%s\",\"has_file\":%s,\"bytes\":%ld,"
        "\"tts_text\":\"%s\","
        "\"tts\":{\"volume\":%s,\"pitch\":%s,\"speed\":%s,\"throat\":%s,\"mouth\":%s}}",
        efirst ? "" : ",",
        id, SOUND_EVENTS[i].label,
        enabled ? "true" : "false",
        file[0] ? file : "",
        has_file ? "true" : "false", bytes,
        tts_esc,
        vol[0] ? vol : "null",
        pitch[0] ? pitch : "null",
        speed[0] ? speed : "null",
        throat[0] ? throat : "null",
        mouth[0] ? mouth : "null");
      efirst = 0;
    }
    eo += (size_t)snprintf(events + eo, sizeof events - eo, "]");

    /* TTS defaults from speak.env */
    int volume = 35, pitch = 64, speed = 72, throat = 128, mouth = 128, speaken = 0;
    {
      FILE *ef = fopen("/mnt/data/nanobot/speak.env", "r");
      if (ef) {
        char line[256];
        while (fgets(line, sizeof line, ef)) {
          if (!strncmp(line, "SPEAK_ENABLED=", 14))
            speaken = (line[14] == '1' || !strncmp(line + 14, "on", 2) || !strncmp(line + 14, "true", 4));
          else if (!strncmp(line, "SPEAK_VOLUME=", 13)) volume = atoi(line + 13);
          else if (!strncmp(line, "SAM_PITCH=", 10)) pitch = atoi(line + 10);
          else if (!strncmp(line, "SAM_SPEED=", 10)) speed = atoi(line + 10);
          else if (!strncmp(line, "SAM_THROAT=", 11)) throat = atoi(line + 11);
          else if (!strncmp(line, "SAM_MOUTH=", 10)) mouth = atoi(line + 10);
        }
        fclose(ef);
      }
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    char body[7200];
    snprintf(body, sizeof body,
      "{\"ok\":true,\"dir\":\"%s\","
      "\"files\":%s,\"events\":%s,"
      "\"tts_defaults\":{\"enabled\":%s,\"volume\":%d,\"pitch\":%d,\"speed\":%d,\"throat\":%d,\"mouth\":%d},"
      "\"limits\":{\"max_upload_bytes\":%d,\"formats\":[\"wav\"]},"
      "\"note\":\"select file or TTS-record per event; ouch plays configured wav on bumper\"}",
      SOUNDS_DIR, files, events,
      speaken ? "true" : "false", volume, pitch, speed, throat, mouth,
      SOUNDS_MAX_UPLOAD);
    resp(cfd, 200, "application/json", body);

  } else if ((is_post || is_put) && !strcmp(path, "/api/v1/sounds")) {
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    sounds_ensure_dirs();
    char action[24] = {0}, event[40] = {0}, file[96] = {0}, text[220] = {0};
    json_str(body, "action", action, sizeof action);
    json_str(body, "event", event, sizeof event);
    if (!event[0]) json_str(body, "id", event, sizeof event);
    json_str(body, "file", file, sizeof file);
    if (!file[0]) json_str(body, "name", file, sizeof file);
    json_str(body, "text", text, sizeof text);
    if (!text[0]) json_str(body, "tts_text", text, sizeof text);

    int volume = 35, pitch = 64, speed = 72, throat = 128, mouth = 128;
    {
      FILE *ef = fopen("/mnt/data/nanobot/speak.env", "r");
      if (ef) {
        char line[256];
        while (fgets(line, sizeof line, ef)) {
          if (!strncmp(line, "SPEAK_VOLUME=", 13)) volume = atoi(line + 13);
          else if (!strncmp(line, "SAM_PITCH=", 10)) pitch = atoi(line + 10);
          else if (!strncmp(line, "SAM_SPEED=", 10)) speed = atoi(line + 10);
          else if (!strncmp(line, "SAM_THROAT=", 11)) throat = atoi(line + 11);
          else if (!strncmp(line, "SAM_MOUTH=", 10)) mouth = atoi(line + 10);
        }
        fclose(ef);
      }
    }
    json_int(body, "volume", &volume);
    json_int(body, "pitch", &pitch);
    json_int(body, "speed", &speed);
    json_int(body, "throat", &throat);
    json_int(body, "mouth", &mouth);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    int play = 1;
    {
      const char *w = strstr(body, "\"play\"");
      if (w) {
        w = strchr(w, ':');
        if (w) {
          while (*w && (*w == ':' || *w == ' ' || *w == '\t')) w++;
          if (!strncmp(w, "false", 5) || w[0] == '0') play = 0;
        }
      }
    }
    int has_enabled = 0, enabled = 1;
    {
      const char *e = strstr(body, "\"enabled\"");
      if (e) {
        e = strchr(e, ':');
        if (e) {
          while (*e && (*e == ':' || *e == ' ' || *e == '\t')) e++;
          has_enabled = 1;
          if (!strncmp(e, "false", 5) || e[0] == '0') enabled = 0;
          else enabled = 1;
        }
      }
    }

    /* action aliases from fields alone */
    if (!action[0]) {
      if (text[0] && event[0]) snprintf(action, sizeof action, "record");
      else if (file[0] && event[0]) snprintf(action, sizeof action, "select");
      else if (event[0] && has_enabled) snprintf(action, sizeof action, "config");
      else if (file[0] || event[0]) snprintf(action, sizeof action, "play");
    }

    char evt[40];
    if (event[0] && safe_event_id(event, evt, sizeof evt) != 0) {
      resp(cfd, 400, "application/json", "{\"error\":\"bad event id\"}");
      free(req); close(cfd); return;
    }
    if (!event[0]) evt[0] = 0;

    if (!strcmp(action, "config") || !strcmp(action, "select") || !strcmp(action, "enable") ||
        !strcmp(action, "disable")) {
      if (!evt[0]) {
        resp(cfd, 400, "application/json", "{\"error\":\"need event\"}");
        free(req); close(cfd); return;
      }
      if (!strcmp(action, "enable")) { has_enabled = 1; enabled = 1; }
      if (!strcmp(action, "disable")) { has_enabled = 1; enabled = 0; }
      if (has_enabled) {
        sounds_write_evt(evt, "enabled", enabled ? "1" : "0");
        if (!strcmp(evt, "ouch")) sounds_sync_ouch_enabled(enabled ? "1" : "0");
      }
      if (file[0] || !strcmp(action, "select")) {
        char safe[96];
        if (file[0] && safe_track_name(file, safe, sizeof safe) != 0) {
          resp(cfd, 400, "application/json", "{\"error\":\"need safe .wav file name\"}");
          free(req); close(cfd); return;
        }
        if (file[0]) {
          char full[320];
          snprintf(full, sizeof full, "%s/%s", SOUNDS_DIR, safe);
          if (access(full, R_OK) != 0) {
            resp(cfd, 404, "application/json", "{\"error\":\"file not in sounds library\"}");
            free(req); close(cfd); return;
          }
          sounds_write_evt(evt, "file", safe);
          /* ouch_watcher default path still ouch.wav — write active pointer + keep symlink alias */
          if (!strcmp(evt, "ouch")) {
            write_small("/mnt/data/rockctl/sounds/ouch.active", safe);
          }
        }
      }
      if (text[0]) sounds_write_evt(evt, "tts_text", text);
      {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", volume); sounds_write_evt(evt, "tts_volume", buf);
        snprintf(buf, sizeof buf, "%d", pitch); sounds_write_evt(evt, "tts_pitch", buf);
        snprintf(buf, sizeof buf, "%d", speed); sounds_write_evt(evt, "tts_speed", buf);
        snprintf(buf, sizeof buf, "%d", throat); sounds_write_evt(evt, "tts_throat", buf);
        snprintf(buf, sizeof buf, "%d", mouth); sounds_write_evt(evt, "tts_mouth", buf);
      }
      char out[240];
      char curfile[96];
      sounds_read_evt(evt, "file", curfile, sizeof curfile, "");
      snprintf(out, sizeof out,
        "{\"ok\":true,\"saved\":true,\"event\":\"%s\",\"file\":\"%s\",\"enabled\":%s}",
        evt, curfile, enabled ? "true" : "false");
      resp(cfd, 200, "application/json", out);
      free(req); close(cfd); return;
    }

    if (!strcmp(action, "play")) {
      char wavpath[320] = {0};
      if (file[0]) {
        char safe[96];
        if (safe_track_name(file, safe, sizeof safe) != 0) {
          resp(cfd, 400, "application/json", "{\"error\":\"bad file name\"}");
          free(req); close(cfd); return;
        }
        snprintf(wavpath, sizeof wavpath, "%s/%s", SOUNDS_DIR, safe);
      } else if (evt[0]) {
        char f[96];
        sounds_read_evt(evt, "file", f, sizeof f, !strcmp(evt, "ouch") ? "ouch.wav" : "");
        if (!f[0]) {
          resp(cfd, 404, "application/json", "{\"error\":\"event has no file\"}");
          free(req); close(cfd); return;
        }
        snprintf(wavpath, sizeof wavpath, "%s/%s", SOUNDS_DIR, f);
      } else {
        resp(cfd, 400, "application/json", "{\"error\":\"need event or file\"}");
        free(req); close(cfd); return;
      }
      if (access(wavpath, R_OK) != 0) {
        resp(cfd, 404, "application/json", "{\"error\":\"wav not found\"}");
        free(req); close(cfd); return;
      }
      {
        char cmd[640];
        snprintf(cmd, sizeof cmd,
          "HOME=/mnt/data/rockctl LD_LIBRARY_PATH=/mnt/data/audio-bin/lib "
          "/mnt/data/audio-bin/bin/aplay -D clanker '%s' >/dev/null 2>&1 &",
          wavpath);
        /* wavpath is under SOUNDS_DIR with safe name only — no shell metachar */
        run_sh(cmd);
      }
      resp(cfd, 200, "application/json",
           "{\"ok\":true,\"played\":true,\"where\":\"robot\"}");
      free(req); close(cfd); return;
    }

    if (!strcmp(action, "record")) {
      if (!evt[0]) {
        resp(cfd, 400, "application/json", "{\"error\":\"need event for record\"}");
        free(req); close(cfd); return;
      }
      if (!text[0]) {
        sounds_read_evt(evt, "tts_text", text, sizeof text, "");
      }
      if (!text[0]) {
        resp(cfd, 400, "application/json", "{\"error\":\"need text to record\"}");
        free(req); close(cfd); return;
      }
      /* default out name: <event>_tts.wav or provided file */
      char safe[96];
      if (file[0]) {
        if (safe_track_name(file, safe, sizeof safe) != 0) {
          resp(cfd, 400, "application/json", "{\"error\":\"bad output file name\"}");
          free(req); close(cfd); return;
        }
      } else {
        snprintf(safe, sizeof safe, "%s_tts.wav", evt);
      }
      char outpath[320];
      snprintf(outpath, sizeof outpath, "%s/%s", SOUNDS_DIR, safe);
      /* write text to temp for stdin */
      char inpath[72];
      snprintf(inpath, sizeof inpath, "/dev/shm/samrec.%u.in", (unsigned)getpid());
      {
        FILE *tf = fopen(inpath, "w");
        if (!tf) {
          resp(cfd, 500, "application/json", "{\"error\":\"cannot write temp text\"}");
          free(req); close(cfd); return;
        }
        fputs(text, tf);
        fclose(tf);
      }
      char recbin[128];
      if (access("/mnt/data/rockctl/bin/sam_record.sh", X_OK) == 0)
        snprintf(recbin, sizeof recbin, "/mnt/data/rockctl/bin/sam_record.sh");
      else if (access("/mnt/data/nanobot-wrapper/speak/sam_record.sh", X_OK) == 0)
        snprintf(recbin, sizeof recbin, "/mnt/data/nanobot-wrapper/speak/sam_record.sh");
      else {
        /* fall back: inline SAM via speak home */
        snprintf(recbin, sizeof recbin, "/mnt/data/rockctl/bin/sam_record.sh");
      }
      if (access(recbin, X_OK) != 0) {
        unlink(inpath);
        resp(cfd, 503, "application/json",
             "{\"error\":\"sam_record.sh not installed on robot\"}");
        free(req); close(cfd); return;
      }
      {
        char cmd[1200];
        snprintf(cmd, sizeof cmd,
          "HOME=/mnt/data/rockctl "
          "SPEAK_ENV=/mnt/data/nanobot/speak.env "
          "SPEAK_HOME=/mnt/data/nanobot-wrapper/speak "
          "SPEAK_ALSA_DEVICE=clanker "
          "PATH=/mnt/data/nanobot-wrapper/speak/bin:/mnt/data/audio-bin/bin:/usr/bin:/bin "
          "LD_LIBRARY_PATH=/mnt/data/audio-bin/lib "
          "SAM_BIN=/mnt/data/nanobot-wrapper/speak/bin/sam "
          "APLAY_BIN=/mnt/data/audio-bin/bin/aplay "
          "WAV_VOL_BIN=/mnt/data/nanobot-wrapper/speak/bin/wav_vol "
          "SPEAK_TMPDIR=/dev/shm "
          "%s --out %s %s --volume %d --pitch %d --speed %d --throat %d --mouth %d "
          "<%s >>/mnt/data/nanobot-wrapper/speak/speak.log 2>&1; "
          "rc=$?; rm -f %s; exit $rc",
          recbin, outpath, play ? "--play" : "",
          volume, pitch, speed, throat, mouth, inpath, inpath);
        int rc = run_sh(cmd);
        if (rc != 0 || access(outpath, R_OK) != 0) {
          char err[200];
          snprintf(err, sizeof err,
            "{\"ok\":false,\"error\":\"record failed (exit %d) — see speak.log\"}", rc);
          resp(cfd, 500, "application/json", err);
          free(req); close(cfd); return;
        }
      }
      /* save event binding + last TTS params */
      sounds_write_evt(evt, "file", safe);
      sounds_write_evt(evt, "tts_text", text);
      {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", volume); sounds_write_evt(evt, "tts_volume", buf);
        snprintf(buf, sizeof buf, "%d", pitch); sounds_write_evt(evt, "tts_pitch", buf);
        snprintf(buf, sizeof buf, "%d", speed); sounds_write_evt(evt, "tts_speed", buf);
        snprintf(buf, sizeof buf, "%d", throat); sounds_write_evt(evt, "tts_throat", buf);
        snprintf(buf, sizeof buf, "%d", mouth); sounds_write_evt(evt, "tts_mouth", buf);
      }
      if (has_enabled) {
        sounds_write_evt(evt, "enabled", enabled ? "1" : "0");
        if (!strcmp(evt, "ouch")) sounds_sync_ouch_enabled(enabled ? "1" : "0");
      }
      if (!strcmp(evt, "ouch"))
        write_small("/mnt/data/rockctl/sounds/ouch.active", safe);
      struct stat st; long bytes = 0;
      if (stat(outpath, &st) == 0) bytes = (long)st.st_size;
      char out[320];
      snprintf(out, sizeof out,
        "{\"ok\":true,\"recorded\":true,\"event\":\"%s\",\"file\":\"%s\",\"bytes\":%ld,"
        "\"played\":%s,\"volume\":%d,\"pitch\":%d,\"speed\":%d,\"throat\":%d,\"mouth\":%d}",
        evt, safe, bytes, play ? "true" : "false",
        volume, pitch, speed, throat, mouth);
      resp(cfd, 200, "application/json", out);
      free(req); close(cfd); return;
    }

    resp(cfd, 400, "application/json",
         "{\"error\":\"action config|select|play|record|enable|disable\"}");
    free(req); close(cfd); return;

  } else if (is_post && !strcmp(path, "/api/v1/sounds/upload")) {
    /* Raw WAV body for event sound library */
    char name[96] = {0};
    const char *sp1 = strchr(req, ' ');
    const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp1 && sp2) {
      for (const char *q = sp1; q < sp2; q++) {
        if (q[0] == '?' || (q[0] == 'n' && strncmp(q, "name=", 5) == 0)) {
          const char *np = strstr(q, "name=");
          if (np && np < sp2) {
            np += 5; size_t i = 0;
            while (np[i] && np[i] != '&' && np[i] != ' ' && i + 1 < sizeof name) {
              name[i] = np[i]; i++;
            }
            name[i] = 0;
          }
          break;
        }
      }
    }
    if (!name[0]) {
      const char *h = strstr(req, "X-Filename:");
      if (!h) h = strstr(req, "x-filename:");
      if (h) {
        h = strchr(h, ':');
        if (h) {
          h++;
          while (*h == ' ' || *h == '\t') h++;
          size_t i = 0;
          while (h[i] && h[i] != '\r' && h[i] != '\n' && i + 1 < sizeof name) {
            name[i] = h[i]; i++;
          }
          name[i] = 0;
        }
      }
    }
    char safe[96];
    if (safe_track_name(name, safe, sizeof safe) != 0) {
      resp(cfd, 400, "application/json",
           "{\"error\":\"need safe name ending in .wav (?name=clip.wav)\"}");
      free(req); close(cfd); return;
    }
    size_t cl = 0;
    const char *clh = strstr(req, "Content-Length:");
    if (!clh) clh = strstr(req, "content-length:");
    if (clh) cl = (size_t)strtoul(clh + 15, NULL, 10);
    if (cl < 44 || cl > SOUNDS_MAX_UPLOAD) {
      resp(cfd, 400, "application/json",
           "{\"error\":\"bad Content-Length (wav 44B..512KB)\"}");
      free(req); close(cfd); return;
    }
    char *hdrend = strstr(req, "\r\n\r\n");
    if (!hdrend) {
      resp(cfd, 400, "application/json", "{\"error\":\"bad request\"}");
      free(req); close(cfd); return;
    }
    char *bdy = hdrend + 4;
    size_t hlen = (size_t)(bdy - req);
    size_t available = (req_len > hlen) ? req_len - hlen : 0;
    if (available < cl) {
      resp(cfd, 400, "application/json", "{\"error\":\"incomplete body\"}");
      free(req); close(cfd); return;
    }
    sounds_ensure_dirs();
    char pathm[320];
    snprintf(pathm, sizeof pathm, "%s/%s", SOUNDS_DIR, safe);
    FILE *wf = fopen(pathm, "wb");
    if (!wf) {
      resp(cfd, 500, "application/json", "{\"error\":\"write failed\"}");
      free(req); close(cfd); return;
    }
    size_t wr = fwrite(bdy, 1, cl, wf);
    fclose(wf);
    if (wr != cl) {
      unlink(pathm);
      resp(cfd, 500, "application/json", "{\"error\":\"short write\"}");
      free(req); close(cfd); return;
    }
    if (cl >= 12 && !(bdy[0] == 'R' && bdy[1] == 'I' && bdy[2] == 'F' && bdy[3] == 'F' &&
                      bdy[8] == 'W' && bdy[9] == 'A' && bdy[10] == 'V' && bdy[11] == 'E')) {
      unlink(pathm);
      resp(cfd, 400, "application/json", "{\"error\":\"not a WAV file (need RIFF/WAVE)\"}");
      free(req); close(cfd); return;
    }
    char out[280];
    snprintf(out, sizeof out,
      "{\"ok\":true,\"name\":\"%s\",\"bytes\":%zu,"
      "\"limits\":{\"max_upload_bytes\":%d,\"formats\":[\"wav\"]}}",
      safe, cl, SOUNDS_MAX_UPLOAD);
    resp(cfd, 200, "application/json", out);

  } else if (is_del && !strncmp(path, "/api/v1/sounds/files/", 21)) {
    const char *tn = path + 21;
    char safe[96];
    if (safe_track_name(tn, safe, sizeof safe) != 0) {
      resp(cfd, 400, "application/json", "{\"error\":\"bad file name\"}");
    } else {
      /* refuse deleting only file still referenced? allow delete; events may show has_file=false */
      char pathm[320];
      snprintf(pathm, sizeof pathm, "%s/%s", SOUNDS_DIR, safe);
      if (unlink(pathm) != 0 && errno != ENOENT) {
        resp(cfd, 500, "application/json", "{\"error\":\"delete failed\"}");
      } else {
        char out[160];
        snprintf(out, sizeof out, "{\"ok\":true,\"deleted\":\"%s\"}", safe);
        resp(cfd, 200, "application/json", out);
      }
    }

  } else if (is_get && !strcmp(path,"/openapi.yaml")) {
    resp(cfd,200,"application/yaml", OPENAPI_YAML);
  } else if (is_get && (!strcmp(path,"/")||!strcmp(path,"/index.html")||!strcmp(path,"/dash"))) {
    /* ClankerDash UI — no miio path (lazy workers never open UDP for this). */
    resp(cfd,200,"text/html; charset=utf-8", CLANKER_DASH_HTML);
  } else if (is_get && (!strcmp(path,"/api")||!strcmp(path,"/docs"))) {
    resp(cfd,200,"text/plain",
      "rockctl API + ClankerDash UI (non-blocking)\n"
      "  UI   GET /\n"
      "  GET  /api/v1/health|status|train/status|resources\n"
      "  POST /api/v1/control {action}  (async 202 default; sync:true to wait)\n"
      "  POST /api/v1/manual/async {action}  (always non-blocking)\n"
      "  PUT  /api/v1/fan|water  (async 202 default)\n"
      "  POST /api/v1/system {restart_stack|reboot}\n");
  } else {
    resp(cfd,404,"text/plain","not found\n");
  }
  free(req); close(cfd);
}

/*
 * Concurrent HTTP: thread pool + non-blocking accept loop.
 * Main thread: select/accept + schedule_tick (never blocked by miio/speak).
 * Workers: each request gets own miio UDP client (no shared sock races).
 */
#ifndef ROCKCTL_HTTP_THREADS
#define ROCKCTL_HTTP_THREADS 6
#endif
#ifndef ROCKCTL_HTTP_QMAX
#define ROCKCTL_HTTP_QMAX 32
#endif

typedef struct {
  int cfd;
  char host[64];
  int miio_port;
  char token_path[160];
} rock_job;

typedef struct {
  rock_job q[ROCKCTL_HTTP_QMAX];
  int head, tail, n;
  int stop;
  pthread_mutex_t mu;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
} rock_pool;

static void *rock_worker(void *arg) {
  rock_pool *pool = (rock_pool *)arg;
  for (;;) {
    rock_job job;
    pthread_mutex_lock(&pool->mu);
    while (pool->n == 0 && !pool->stop)
      pthread_cond_wait(&pool->not_empty, &pool->mu);
    if (pool->stop && pool->n == 0) {
      pthread_mutex_unlock(&pool->mu);
      break;
    }
    job = pool->q[pool->head];
    pool->head = (pool->head + 1) % ROCKCTL_HTTP_QMAX;
    pool->n--;
    pthread_cond_signal(&pool->not_full);
    pthread_mutex_unlock(&pool->mu);

    miio_client mc;
    memset(&mc, 0, sizeof mc);
    mc.sock = -1;
    /* Lazy miio: only prepare credentials; open UDP on first miio_call.
     * Static routes (/, health, train) never open a socket. */
    snprintf(mc.host, sizeof mc.host, "%s", job.host[0] ? job.host : "127.0.0.1");
    mc.port = job.miio_port > 0 ? job.miio_port : 54321;
    snprintf(mc.token_path, sizeof mc.token_path, "%s",
             job.token_path[0] ? job.token_path : "/mnt/data/miio/device.token");
    mc.token_ok = 0;
    mc.timeout_ms = 1500;
    handle(job.cfd, &mc);
    miio_close(&mc);
  }
  return NULL;
}

static int rock_pool_push(rock_pool *pool, const rock_job *job) {
  pthread_mutex_lock(&pool->mu);
  while (pool->n >= ROCKCTL_HTTP_QMAX && !pool->stop)
    pthread_cond_wait(&pool->not_full, &pool->mu);
  if (pool->stop) {
    pthread_mutex_unlock(&pool->mu);
    return -1;
  }
  pool->q[pool->tail] = *job;
  pool->tail = (pool->tail + 1) % ROCKCTL_HTTP_QMAX;
  pool->n++;
  pthread_cond_signal(&pool->not_empty);
  pthread_mutex_unlock(&pool->mu);
  return 0;
}

int rockctl_http_serve(miio_client *m, int port) {
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
  if (listen(s, 64) != 0) { close(s); return -1; }

  rock_pool pool;
  memset(&pool, 0, sizeof pool);
  pthread_mutex_init(&pool.mu, NULL);
  pthread_cond_init(&pool.not_empty, NULL);
  pthread_cond_init(&pool.not_full, NULL);

  pthread_t th[ROCKCTL_HTTP_THREADS];
  int nthr = 0;
  for (int i = 0; i < ROCKCTL_HTTP_THREADS; i++) {
    if (pthread_create(&th[i], NULL, rock_worker, &pool) == 0)
      nthr++;
  }
  if (nthr == 0) {
    fprintf(stderr, "rockctl: pthread_create failed — inline single-thread mode\n");
  }

  fprintf(stderr, "rockctl http on 0.0.0.0:%d (thread pool %d, queue %d)\n",
          port, nthr, ROCKCTL_HTTP_QMAX);

  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int pr = select(s + 1, &rfds, NULL, NULL, &tv);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    /* Schedule runs on main thread only (never blocked by request handlers) */
    schedule_tick(m);
    if (pr == 0 || !FD_ISSET(s, &rfds)) continue;

    int c = accept(s, NULL, NULL);
    if (c < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (nthr == 0) {
      handle(c, m);
      continue;
    }

    rock_job job;
    memset(&job, 0, sizeof job);
    job.cfd = c;
    snprintf(job.host, sizeof job.host, "%s", m->host[0] ? m->host : "127.0.0.1");
    job.miio_port = m->port > 0 ? m->port : 54321;
    snprintf(job.token_path, sizeof job.token_path, "%s",
             m->token_path[0] ? m->token_path : "/mnt/data/miio/device.token");

    if (rock_pool_push(&pool, &job) != 0) {
      close(c);
    }
  }

  /* shutdown pool */
  pthread_mutex_lock(&pool.mu);
  pool.stop = 1;
  pthread_cond_broadcast(&pool.not_empty);
  pthread_cond_broadcast(&pool.not_full);
  pthread_mutex_unlock(&pool.mu);
  for (int i = 0; i < nthr; i++)
    pthread_join(th[i], NULL);
  pthread_mutex_destroy(&pool.mu);
  pthread_cond_destroy(&pool.not_empty);
  pthread_cond_destroy(&pool.not_full);
  close(s);
  return 0;
}
