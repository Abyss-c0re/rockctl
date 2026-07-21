#ifndef ROCKCTL_MD5_H
#define ROCKCTL_MD5_H
#include <stdint.h>
#include <stddef.h>
void md5(const uint8_t *data, size_t len, uint8_t out[16]);
#endif
