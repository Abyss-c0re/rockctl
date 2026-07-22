#include "miio.h"
#include "http.h"
#include "places.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* miio_prepare / miio_ensure: serve without hard-failing boot race */

static const char *default_places_path(void) {
  if (access("/mnt/data/rockctl", W_OK) == 0)
    return "/mnt/data/rockctl/places.json";
  return "/tmp/rockctl_places.json";
}

static const char *default_charger_path(void) {
  if (access("/mnt/data/rockrobo/ChargerPos.data", R_OK) == 0)
    return "/mnt/data/rockrobo/ChargerPos.data";
  return "/mnt/data/rockrobo/ChargerPos.data";
}

static void usage(const char *a0) {
  fprintf(stderr,
    "rockctl 0.1.1 — local Roborock control (miio), from source\n\n"
    "Usage:\n"
    "  %s status|start|stop|pause|home|spot|locate\n"
    "  %s fan quiet|balanced|turbo|max\n"
    "  %s water off|low|medium|high   # mop moisture (water box)\n"
    "  %s consumable\n"
    "  %s goto <x> <y>           # drive only (app_goto_target, no clean)\n"
    "  %s place list|set|go ...\n"
    "  %s demo work-room         # DEMO: drive to work room, no cleaning\n"
    "  %s raw <method> [params]\n"
    "  %s serve [--port N]\n\n"
    "place set <name> <x> <y> [label...]\n"
    "place go <name>\n\n"
    "Defaults: host=127.0.0.1 token=/mnt/data/miio/device.token\n",
    a0, a0, a0, a0, a0, a0, a0, a0, a0);
}

static int cmd_goto(miio_client *m, int x, int y, char **result) {
  /* Ensure not in a clean cycle: stop first (ignore failure) */
  char *tmp = NULL;
  miio_call(m, "app_stop", "[]", &tmp);
  free(tmp);
  tmp = NULL;
  miio_call(m, "app_pause", "[]", &tmp);
  free(tmp);

  char params[64];
  snprintf(params, sizeof params, "[%d,%d]", x, y);
  fprintf(stderr, "rockctl: drive-only goto [%d, %d] (no clean)\n", x, y);
  return miio_call(m, "app_goto_target", params, result);
}

static int load_places(rock_places *pl) {
  const char *path = getenv("ROCKCTL_PLACES");
  if (!path || !path[0]) path = default_places_path();
  if (places_load(pl, path) != 0) {
    memset(pl, 0, sizeof *pl);
    snprintf(pl->path, sizeof pl->path, "%s", path);
  }
  places_ensure_defaults(pl, default_charger_path());
  return 0;
}

int main(int argc, char **argv) {
  const char *host = "127.0.0.1";
  const char *token = "/mnt/data/miio/device.token";
  int miio_port = 54321;
  int http_port = 8080;
  int i = 1;
  while (i < argc && argv[i][0] == '-') {
    if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
    else if (!strcmp(argv[i], "--token") && i + 1 < argc) token = argv[++i];
    else if (!strcmp(argv[i], "--miio-port") && i + 1 < argc) miio_port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) http_port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "unknown %s\n", argv[i]);
      return 2;
    }
    i++;
  }
  if (i >= argc) {
    usage(argv[0]);
    return 2;
  }
  const char *cmd = argv[i++];

  miio_client m;
  memset(&m, 0, sizeof m);
  m.sock = -1;

  int rc = 0;
  char *result = NULL;
  rock_places places;
  load_places(&places);

  if (!strcmp(cmd, "serve")) {
    while (i < argc) {
      if (!strcmp(argv[i], "--port") && i + 1 < argc) http_port = atoi(argv[++i]);
      i++;
    }
    /* Never exit just because miio hello failed at boot — HTTP/dash must stay up.
     * Firmware miio often comes late; miio_call will miio_ensure() lazily. */
    if (miio_prepare(&m, host, miio_port, token) != 0) {
      fprintf(stderr, "miio token missing (%s) — serve still starts; control calls will fail\n", token);
    } else if (miio_ensure(&m) != 0) {
      fprintf(stderr, "miio hello not ready yet (host=%s) — HTTP up, will retry on demand\n", host);
    }
    fprintf(stderr, "rockctl serving API on :%d\n", http_port);
    rc = rockctl_http_serve(&m, http_port);
    miio_close(&m);
  } else if (miio_open(&m, host, miio_port, token) != 0) {
    /* One-shot CLI still needs a live miio handshake. */
    fprintf(stderr, "miio_open failed (host=%s token=%s)\n", host, token);
    return 1;
  } else if (!strcmp(cmd, "status")) {
    rc = miio_call(&m, "get_status", "[]", &result);
  } else if (!strcmp(cmd, "start")) {
    rc = miio_call(&m, "app_start", "[]", &result);
  } else if (!strcmp(cmd, "stop")) {
    rc = miio_call(&m, "app_stop", "[]", &result);
  } else if (!strcmp(cmd, "pause")) {
    rc = miio_call(&m, "app_pause", "[]", &result);
  } else if (!strcmp(cmd, "home") || !strcmp(cmd, "dock")) {
    /* Drop RC first — app_charge often ignored while manual/RC is active */
    free(result);
    result = NULL;
    miio_call(&m, "app_rc_end", "[]", &result);
    free(result);
    result = NULL;
    miio_call(&m, "app_pause", "[]", &result);
    free(result);
    result = NULL;
    rc = miio_call(&m, "app_charge", "[]", &result);
  } else if (!strcmp(cmd, "spot")) {
    rc = miio_call(&m, "app_spot", "[]", &result);
  } else if (!strcmp(cmd, "locate") || !strcmp(cmd, "find")) {
    rc = miio_call(&m, "find_me", "[]", &result);
  } else if (!strcmp(cmd, "consumable")) {
    rc = miio_call(&m, "get_consumable", "[]", &result);
  } else if (!strcmp(cmd, "fan") && i < argc) {
    const char *lvl = argv[i++];
    int mode = -1;
    if (!strcmp(lvl, "quiet") || !strcmp(lvl, "silent")) mode = 101;
    else if (!strcmp(lvl, "balanced") || !strcmp(lvl, "normal") || !strcmp(lvl, "medium"))
      mode = 102;
    else if (!strcmp(lvl, "turbo")) mode = 103;
    else if (!strcmp(lvl, "max")) mode = 104;
    if (mode < 0) {
      fprintf(stderr, "bad fan level\n");
      rc = 2;
    } else {
      char p[16];
      snprintf(p, sizeof p, "[%d]", mode);
      rc = miio_call(&m, "set_custom_mode", p, &result);
    }
  } else if ((!strcmp(cmd, "water") || !strcmp(cmd, "moist") || !strcmp(cmd, "mop")) && i < argc) {
    const char *lvl = argv[i++];
    int mode = -1;
    if (!strcmp(lvl, "off") || !strcmp(lvl, "none") || !strcmp(lvl, "dry")) mode = 200;
    else if (!strcmp(lvl, "low") || !strcmp(lvl, "min") || !strcmp(lvl, "quiet")) mode = 201;
    else if (!strcmp(lvl, "medium") || !strcmp(lvl, "med") || !strcmp(lvl, "normal") ||
             !strcmp(lvl, "balanced"))
      mode = 202;
    else if (!strcmp(lvl, "high") || !strcmp(lvl, "max") || !strcmp(lvl, "wet")) mode = 203;
    if (mode < 0) {
      fprintf(stderr, "bad water/moist level (off|low|medium|high)\n");
      rc = 2;
    } else {
      char p[16];
      snprintf(p, sizeof p, "[%d]", mode);
      rc = miio_call(&m, "set_water_box_custom_mode", p, &result);
    }
  } else if (!strcmp(cmd, "goto") && i + 1 < argc) {
    int x = atoi(argv[i++]);
    int y = atoi(argv[i++]);
    rc = cmd_goto(&m, x, y, &result);
  } else if (!strcmp(cmd, "place") && i < argc) {
    const char *sub = argv[i++];
    if (!strcmp(sub, "list")) {
      places_list(&places);
      rc = 0;
    } else if (!strcmp(sub, "set") && i + 2 < argc) {
      const char *name = argv[i++];
      int x = atoi(argv[i++]);
      int y = atoi(argv[i++]);
      char label[96] = "";
      if (i < argc) {
        size_t off = 0;
        while (i < argc && off + 2 < sizeof label) {
          size_t n = snprintf(label + off, sizeof label - off, "%s%s", off ? " " : "", argv[i++]);
          off += n;
        }
      }
      rc = places_set(&places, name, x, y, label[0] ? label : name, 1);
      if (rc == 0)
        printf("{\"ok\":true,\"name\":\"%s\",\"x\":%d,\"y\":%d,\"calibrated\":true}\n", name, x,
               y);
    } else if (!strcmp(sub, "go") && i < argc) {
      const char *name = argv[i++];
      rock_place pl;
      if (places_get(&places, name, &pl) != 0) {
        fprintf(stderr, "unknown place: %s (rockctl place list)\n", name);
        rc = 2;
      } else {
        fprintf(stderr, "place %s (%s) -> [%d,%d] calibrated=%d\n", pl.name, pl.label, pl.x, pl.y,
                pl.calibrated);
        rc = cmd_goto(&m, pl.x, pl.y, &result);
      }
    } else {
      usage(argv[0]);
      rc = 2;
    }
  } else if (!strcmp(cmd, "demo") && i < argc) {
    const char *what = argv[i++];
    if (!strcmp(what, "work-room") || !strcmp(what, "work_room") || !strcmp(what, "workroom")) {
      rock_place pl;
      if (places_get(&places, "work_room", &pl) != 0) {
        places_ensure_defaults(&places, default_charger_path());
        places_get(&places, "work_room", &pl);
      }
      fprintf(stderr,
              "DEMO: drive to work room without cleaning\n"
              "  target [%d, %d] (%s)\n"
              "  calibrate later: rockctl place set work_room <x> <y>\n",
              pl.x, pl.y, pl.calibrated ? "calibrated" : "default from dock offset");
      rc = cmd_goto(&m, pl.x, pl.y, &result);
    } else {
      fprintf(stderr, "unknown demo: %s (try: work-room)\n", what);
      rc = 2;
    }
  } else if (!strcmp(cmd, "raw") && i < argc) {
    const char *method = argv[i++];
    const char *params = (i < argc) ? argv[i++] : "[]";
    rc = miio_call(&m, method, params, &result);
  } else {
    usage(argv[0]);
    rc = 2;
  }

  if (result) {
    puts(result);
    free(result);
  } else if (rc != 0 && strcmp(cmd, "serve") && strcmp(cmd, "place")) {
    fprintf(stderr, "command failed (rc=%d)\n", rc);
  }

  miio_close(&m);
  return rc ? 1 : 0;
}
