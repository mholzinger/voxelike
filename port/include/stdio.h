#ifndef PORT_STDIO_H
#define PORT_STDIO_H

/* Minimal stdio for a console with no files. Formatting is real; anything
 * that would touch a file or a terminal stream does nothing. */
#include <stddef.h>
#include <stdarg.h>

typedef struct port_file FILE;
extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)

int   sprintf(char *buf, const char *fmt, ...);
int   vsprintf(char *buf, const char *fmt, va_list ap);
int   printf(const char *fmt, ...);
int   putchar(int c);
int   getchar(void);
int   fflush(FILE *f);
char *fgets(char *s, int n, FILE *f);

#endif
