#ifndef __STRING_H__
#define __STRING_H__

#include <stddef.h>

/* Implemented in port/libc.c, the one C library in the ROM. */

#define isdigit(c)		((c) >= '0' && (c) <= '9')

size_t strlen(const char *str);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

#endif
