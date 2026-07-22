#include "drive_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

void drive_path_init(void) {
  mkdir("/mnt/data/rockctl", 0755);
  mkdir(DRIVE_PATH_DIR, 0755);
}

int drive_path_read_last(char *buf, size_t cap) {
  FILE *f;
  size_t n;
  if (!buf || cap < 4) return -1;
  drive_path_init();
  f = fopen(DRIVE_PATH_LAST, "r");
  if (!f) {
    snprintf(buf, cap, "{\"ok\":true,\"points\":[],\"n\":0,\"note\":\"no last drive yet\"}");
    return (int)strlen(buf);
  }
  n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return (int)n;
}

int drive_path_write_last(const char *json, size_t n) {
  FILE *f;
  if (!json || !n) return -1;
  drive_path_init();
  f = fopen(DRIVE_PATH_LAST, "w");
  if (!f) return -1;
  if (fwrite(json, 1, n, f) != n) { fclose(f); return -1; }
  fclose(f);
  /* also archive a timestamped copy (best-effort) */
  {
    char arch[128];
    FILE *a;
    snprintf(arch, sizeof arch, "%s/drive_%ld.json", DRIVE_PATH_DIR, (long)time(NULL));
    a = fopen(arch, "w");
    if (a) { fwrite(json, 1, n, a); fclose(a); }
  }
  return 0;
}

static int read_bin(const char *path, unsigned char **out, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  long sz;
  unsigned char *b;
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  sz = ftell(f);
  if (sz < 50 || sz > 8 * 1024 * 1024) { fclose(f); return -1; }
  rewind(f);
  b = (unsigned char *)malloc((size_t)sz);
  if (!b) { fclose(f); return -1; }
  if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
  fclose(f);
  *out = b;
  *out_len = (size_t)sz;
  return 0;
}

/* RRSLAM: LE int32 at 0x16 width, 0x1a height, data @ 46 */
int drive_path_extract_rrslam(char *buf, size_t cap, int max_pts) {
  unsigned char *bin = NULL;
  size_t bl = 0;
  int w, h, off = 46;
  int x, y, n = 0, o = 0;
  int step;
  if (!buf || cap < 64) return -1;
  if (max_pts < 16) max_pts = 16;
  if (max_pts > 1500) max_pts = 1500;
  if (read_bin("/mnt/data/rockrobo/last_map", &bin, &bl) != 0 &&
      read_bin("/mnt/data/rockrobo/user_map0", &bin, &bl) != 0) {
    snprintf(buf, cap, "{\"ok\":false,\"error\":\"no map\"}");
    return (int)strlen(buf);
  }
  if (bl < 50 || bin[0] != 'R' || bin[1] != 'R') {
    free(bin);
    snprintf(buf, cap, "{\"ok\":false,\"error\":\"bad map\"}");
    return (int)strlen(buf);
  }
  w = (int)(bin[0x16] | (bin[0x17] << 8) | (bin[0x18] << 16) | (bin[0x19] << 24));
  h = (int)(bin[0x1a] | (bin[0x1b] << 8) | (bin[0x1c] << 16) | (bin[0x1d] << 24));
  if (w < 32 || h < 32 || w > 4096 || h > 4096 || (size_t)(off + w * h) > bl) {
    free(bin);
    snprintf(buf, cap, "{\"ok\":false,\"error\":\"bad dims\"}");
    return (int)strlen(buf);
  }
  /* subsample path cells so we fit */
  step = 1;
  {
    int count = 0;
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
        unsigned char v = bin[off + y * w + x];
        if (v >= 0x01 && v <= 0x1f) count++;
      }
    if (count > max_pts) step = (count + max_pts - 1) / max_pts;
    if (step < 1) step = 1;
  }
  o += snprintf(buf + o, cap - (size_t)o,
                "{\"ok\":true,\"source\":\"rrslam\",\"width\":%d,\"height\":%d,\"step\":%d,\"points\":[",
                w, h, step);
  for (y = 0; y < h && n < max_pts && o + 48 < (int)cap; y++) {
    for (x = 0; x < w && n < max_pts && o + 48 < (int)cap; x++) {
      unsigned char v = bin[off + y * w + x];
      if (v < 0x01 || v > 0x1f) continue;
      if (((x + y) % step) != 0 && v > 0x07) continue; /* keep all fresh; thin old */
      o += snprintf(buf + o, cap - (size_t)o, "%s{\"cx\":%d,\"cy\":%d,\"v\":%u}",
                    n ? "," : "", x, y, (unsigned)v);
      n++;
    }
  }
  free(bin);
  o += snprintf(buf + o, cap - (size_t)o, "],\"n\":%d}", n);
  return o;
}
