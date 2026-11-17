#ifndef PORT_STDLIB_H
#define PORT_STDLIB_H

#include <stddef.h>

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void  free(void *p);
int   atoi(const char *s);
int   abs(int v);
char *getenv(const char *name);
void  exit(int status);
void  abort(void);

#endif
