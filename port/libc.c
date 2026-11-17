/* The slice of the C library Rogue uses, for a toolchain that ships none.
 * Built with -fno-builtin so nothing here compiles into a call to itself. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>

/* --- strings -------------------------------------------------------- */

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++) != 0);
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && (*d = *s) != 0) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strstr(const char *s, const char *word) {
    size_t n = strlen(word);
    for (; *s; s++) {
        if (strncmp(s, word, n) == 0) return (char *)s;
    }
    return n ? NULL : (char *)s;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* --- ctype ---------------------------------------------------------- */

int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isprint(int c) { return c >= ' ' && c < 127; }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
int toascii(int c) { return c & 0x7F; }

/* --- stdlib --------------------------------------------------------- */

int abs(int v) { return v < 0 ? -v : v; }

int atoi(const char *s) {
    int v = 0, neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

/* First-fit heap with forward coalescing. Rogue allocates a few dozen
 * small monster and item records per level and frees them on the next. */
#define HEAP_SIZE 32768

typedef struct block {
    size_t size;            /* payload bytes, multiple of 8 */
    size_t used;
} block_t;

static union { block_t first; unsigned char bytes[HEAP_SIZE]; } heap;
static int heap_ready;

#define HEAP_END   ((block_t *)(heap.bytes + HEAP_SIZE))
#define NEXT(b)    ((block_t *)((unsigned char *)((b) + 1) + (b)->size))

void *malloc(size_t n) {
    if (!heap_ready) {
        heap.first.size = HEAP_SIZE - sizeof(block_t);
        heap.first.used = 0;
        heap_ready = 1;
    }
    n = (n + 7) & ~(size_t)7;
    if (n == 0) n = 8;

    for (block_t *b = &heap.first; b < HEAP_END; b = NEXT(b)) {
        if (b->used) continue;
        while (NEXT(b) < HEAP_END && !NEXT(b)->used)
            b->size += sizeof(block_t) + NEXT(b)->size;
        if (b->size < n) continue;
        if (b->size >= n + sizeof(block_t) + 8) {
            block_t *rest = (block_t *)((unsigned char *)(b + 1) + n);
            rest->size = b->size - n - sizeof(block_t);
            rest->used = 0;
            b->size = n;
        }
        b->used = 1;
        return b + 1;
    }
    return NULL;
}

void *calloc(size_t count, size_t size) {
    size_t n = count * size;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void free(void *p) {
    if (p) ((block_t *)p - 1)->used = 0;
}

/* --- stdio ---------------------------------------------------------- */

FILE *stdin, *stdout, *stderr;

static char *put_uint(char *end, unsigned long v, unsigned base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        *--end = digits[v % base];
        v /= base;
    } while (v);
    return end;
}

/* Handles what Rogue's format strings use: flags - and 0, width and
 * precision (digits or *), h and l, and conversions d i u x X c s %. */
int vsprintf(char *buf, const char *fmt, va_list ap) {
    char *out = buf;

    while (*fmt) {
        if (*fmt != '%') { *out++ = *fmt++; continue; }
        fmt++;

        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt != '+' && *fmt != ' ' && *fmt != '#') break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        } else {
            while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h') { if (*fmt == 'l') lng = 1; fmt++; }

        char tmp[24];
        char *end = tmp + sizeof(tmp);
        const char *s = end;
        int len = 0, neg = 0;
        char conv = *fmt ? *fmt++ : 0;

        switch (conv) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long u = v < 0 ? (neg = 1, 0UL - (unsigned long)v) : (unsigned long)v;
            s = put_uint(end, u, 10, 0);
            len = (int)(end - s);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long u = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            s = put_uint(end, u, conv == 'u' ? 10 : 16, conv == 'X');
            len = (int)(end - s);
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            s = tmp;
            len = 1;
            zero = 0;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            zero = 0;
            break;
        case '%':
            *out++ = '%';
            continue;
        default:
            if (conv) *out++ = conv;
            continue;
        }

        int pad = width - len - neg;
        if (left) zero = 0;
        if (!left && !zero) while (pad-- > 0) *out++ = ' ';
        if (neg) *out++ = '-';
        if (zero) while (pad-- > 0) *out++ = '0';
        while (len--) *out++ = *s++;
        if (left) while (pad-- > 0) *out++ = ' ';
    }
    *out = 0;
    return (int)(out - buf);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

/* There is no terminal behind the game screen, so the few messages Rogue
 * prints outside curses (startup banner, score list) go nowhere for now. */
int printf(const char *fmt, ...) { (void)fmt; return 0; }
int putchar(int c) { return c; }
int getchar(void) { return '\n'; }
int fflush(FILE *f) { (void)f; return 0; }
char *fgets(char *s, int n, FILE *f) { (void)s; (void)n; (void)f; return NULL; }

/* --- signal, time --------------------------------------------------- */

sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig; (void)handler;
    return SIG_DFL;
}

extern unsigned long plat_ticks(void);

time_t time(time_t *t) {
    time_t now = (time_t)plat_ticks();
    if (t) *t = now;
    return now;
}

struct tm *localtime(const time_t *t) {
    static struct tm tm;
    (void)t;
    return &tm;
}

/* --- process -------------------------------------------------------- */

extern void plat_exit(int status);

void exit(int status) {
    plat_exit(status);
    for (;;);
}

void abort(void) {
    exit(1);
}
