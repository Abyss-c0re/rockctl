#ifndef ROCKCTL_PLACES_H
#define ROCKCTL_PLACES_H

typedef struct {
  char name[64];
  char label[96];
  int x;
  int y;
  int calibrated;
} rock_place;

#define ROCK_PLACES_MAX 32

typedef struct {
  rock_place places[ROCK_PLACES_MAX];
  int n;
  char path[256];
} rock_places;

/* Load/save places JSON (simple). path e.g. /mnt/data/rockctl/places.json */
int places_load(rock_places *p, const char *path);
int places_save(const rock_places *p);
int places_get(const rock_places *p, const char *name, rock_place *out);
int places_set(rock_places *p, const char *name, int x, int y, const char *label, int calibrated);
void places_list(const rock_places *p);
/* Ensure work_room exists with dock-derived default if missing */
int places_ensure_defaults(rock_places *p, const char *charger_pos_path);

#endif
