#include "places.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static void ensure_dir(const char *path) {
  char dir[256];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    mkdir(dir, 0755);
  }
}

static int parse_int_field(const char *json, const char *key, int *out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return -1;
  p = strchr(p + strlen(pat), ':');
  if (!p) return -1;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  *out = atoi(p);
  return 0;
}

static int parse_str_field(const char *json, const char *key, char *out, size_t n) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return -1;
  p = strchr(p + strlen(pat), ':');
  if (!p) return -1;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return -1;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
  out[i] = 0;
  return 0;
}

int places_load(rock_places *rp, const char *path) {
  memset(rp, 0, sizeof *rp);
  snprintf(rp->path, sizeof rp->path, "%s", path);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024) { fclose(f); return -1; }
  char *buf = malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return -1; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = 0;

  /* Very simple: find each "name": "work_room" block by scanning for "x": */
  const char *p = buf;
  while (rp->n < ROCK_PLACES_MAX && (p = strstr(p, "\"x\"")) != NULL) {
    /* walk back a bit for name/label */
    const char *block = p - 120;
    if (block < buf) block = buf;
    rock_place pl;
    memset(&pl, 0, sizeof pl);
    if (parse_int_field(block, "x", &pl.x) != 0 || parse_int_field(block, "y", &pl.y) != 0) {
      p += 3;
      continue;
    }
    if (parse_str_field(block, "name", pl.name, sizeof pl.name) != 0) {
      /* try parent key pattern "work_room": { */
      const char *q = block;
      while (q > buf && *q != '"') q--;
      /* fallback */
      snprintf(pl.name, sizeof pl.name, "place%d", rp->n);
    }
    parse_str_field(block, "label", pl.label, sizeof pl.label);
    int cal = 0;
    if (parse_int_field(block, "calibrated", &cal) == 0) pl.calibrated = cal;
    if (!pl.label[0]) snprintf(pl.label, sizeof pl.label, "%s", pl.name);
    rp->places[rp->n++] = pl;
    p += 3;
  }
  free(buf);
  return rp->n > 0 ? 0 : -1;
}

int places_save(const rock_places *rp) {
  ensure_dir(rp->path);
  FILE *f = fopen(rp->path, "w");
  if (!f) return -1;
  fprintf(f, "{\n");
  for (int i = 0; i < rp->n; i++) {
    const rock_place *pl = &rp->places[i];
    fprintf(f,
      "  \"%s\": {\n"
      "    \"name\": \"%s\",\n"
      "    \"label\": \"%s\",\n"
      "    \"x\": %d,\n"
      "    \"y\": %d,\n"
      "    \"calibrated\": %s,\n"
      "    \"method\": \"app_goto_target\",\n"
      "    \"clean\": false\n"
      "  }%s\n",
      pl->name, pl->name, pl->label[0] ? pl->label : pl->name,
      pl->x, pl->y, pl->calibrated ? "true" : "false",
      i + 1 < rp->n ? "," : "");
  }
  fprintf(f, "}\n");
  fclose(f);
  return 0;
}

int places_get(const rock_places *p, const char *name, rock_place *out) {
  for (int i = 0; i < p->n; i++) {
    if (strcmp(p->places[i].name, name) == 0) {
      *out = p->places[i];
      return 0;
    }
  }
  /* also match work-room / workroom aliases */
  if (strcmp(name, "work-room") == 0 || strcmp(name, "workroom") == 0 ||
      strcmp(name, "work") == 0) {
    return places_get(p, "work_room", out);
  }
  return -1;
}

int places_set(rock_places *p, const char *name, int x, int y, const char *label, int calibrated) {
  for (int i = 0; i < p->n; i++) {
    if (strcmp(p->places[i].name, name) == 0) {
      p->places[i].x = x;
      p->places[i].y = y;
      p->places[i].calibrated = calibrated;
      if (label && label[0])
        snprintf(p->places[i].label, sizeof p->places[i].label, "%s", label);
      return places_save(p);
    }
  }
  if (p->n >= ROCK_PLACES_MAX) return -1;
  rock_place *pl = &p->places[p->n++];
  memset(pl, 0, sizeof *pl);
  snprintf(pl->name, sizeof pl->name, "%s", name);
  snprintf(pl->label, sizeof pl->label, "%s", label && label[0] ? label : name);
  pl->x = x;
  pl->y = y;
  pl->calibrated = calibrated;
  return places_save(p);
}

void places_list(const rock_places *p) {
  for (int i = 0; i < p->n; i++) {
    const rock_place *pl = &p->places[i];
    printf("%-16s  x=%d y=%d  %s  %s\n",
           pl->name, pl->x, pl->y,
           pl->calibrated ? "calibrated" : "default",
           pl->label);
  }
}

/* Parse ChargerPos.data "x = N;" style */
static int read_charger_xy(const char *path, int *x, int *y) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[128];
  int gotx = 0, goty = 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "x =", 3) == 0 || strncmp(line, "x=", 2) == 0) {
      char *eq = strchr(line, '=');
      if (eq) { *x = atoi(eq + 1); gotx = 1; }
    }
    if (strncmp(line, "y =", 3) == 0 || strncmp(line, "y=", 2) == 0) {
      char *eq = strchr(line, '=');
      if (eq) { *y = atoi(eq + 1); goty = 1; }
    }
  }
  fclose(f);
  return (gotx && goty) ? 0 : -1;
}

int places_ensure_defaults(rock_places *p, const char *charger_pos_path) {
  rock_place wr;
  if (places_get(p, "work_room", &wr) == 0) return 0;

  /* Convert slam-ish ChargerPos into app_goto_target frame:
   * many stacks use map-center ~25500; local charger (cx,cy) -> (25500+cx, 25500+cy).
   * Work room default: offset from dock into the map (+dx, +dy).
   */
  int cx = 0, cy = 0;
  int wx, wy;
  if (read_charger_xy(charger_pos_path, &cx, &cy) == 0) {
    wx = 25500 + cx + 4500;  /* ~4.5 m toward work area from dock */
    wy = 25500 + cy + 2000;
  } else {
    /* Fallback near map center / open floor — calibrate ASAP */
    wx = 30000;
    wy = 28000;
  }
  return places_set(p, "work_room", wx, wy, "Work room (drive only, no clean)", 0);
}
