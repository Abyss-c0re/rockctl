/* Last-drive path for ClankerDash coverage + LHLAM export. */
#ifndef ROCKCTL_DRIVE_PATH_H
#define ROCKCTL_DRIVE_PATH_H

#include <stddef.h>

#define DRIVE_PATH_DIR   "/mnt/data/rockctl/drive"
#define DRIVE_PATH_LAST  "/mnt/data/rockctl/drive/last.json"
#define DRIVE_PATH_MAX_PTS 2048

/* Ensure dir exists. */
void drive_path_init(void);

/* Read last.json into buf; returns bytes or -1. */
int drive_path_read_last(char *buf, size_t cap);

/* Write full JSON body to last.json (from teach/export). */
int drive_path_write_last(const char *json, size_t n);

/* Extract RRSLAM path cells (0x01-0x1f) from last_map → compact JSON into buf.
 * format: {"ok":true,"source":"rrslam","points":[{"cx":..,"cy":..,"v":..},...],"n":N}
 */
int drive_path_extract_rrslam(char *buf, size_t cap, int max_pts);

#endif
