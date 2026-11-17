#ifndef PORT_STRING_H
#define PORT_STRING_H

#include <stddef.h>

size_t strlen(const char *s);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);

#endif
