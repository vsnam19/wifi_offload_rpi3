/*
 * Minimal os.h stub for standalone wpa_ctrl compilation.
 * Maps os_* typedefs and inline functions to POSIX equivalents.
 */
#ifndef OS_H
#define OS_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>    /* usleep, sleep */
#include <features.h>

typedef long os_time_t;

struct os_reltime {
    os_time_t sec;
    os_time_t usec;
};

static inline int os_get_reltime(struct os_reltime *t) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return -1;
    t->sec  = (os_time_t)tv.tv_sec;
    t->usec = (os_time_t)tv.tv_usec;
    return 0;
}

static inline int os_reltime_expired(struct os_reltime *now,
                                     struct os_reltime *ts,
                                     os_time_t timeout_secs) {
    struct os_reltime age;
    age.sec  = now->sec  - ts->sec;
    age.usec = now->usec - ts->usec;
    if (age.usec < 0) { age.sec--; age.usec += 1000000; }
    return (age.sec > timeout_secs ||
            (age.sec == timeout_secs && age.usec > 0));
}

static inline void os_sleep(os_time_t sec, os_time_t usec) {
    if (sec) sleep((unsigned int)sec);
    if (usec) usleep((unsigned int)usec);
}

#define os_malloc(s)       malloc(s)
#define os_free(p)         free(p)
#define os_memset(p,c,n)   memset(p,c,n)
#define os_memcpy(d,s,n)   memcpy(d,s,n)
#define os_memcmp(a,b,n)   memcmp(a,b,n)
#define os_strlen(s)       strlen(s)
#define os_strcmp(a,b)     strcmp(a,b)
#define os_strncmp(a,b,n)  strncmp(a,b,n)
#define os_strchr(s,c)     strchr(s,c)
#define os_snprintf        snprintf

static inline void * os_zalloc(size_t size) {
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static inline char * os_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static inline size_t os_strlcpy(char *dest, const char *src, size_t siz) {
    const char *s = src;
    size_t n = siz;
    if (n && --n) {
        do { if (!(*dest++ = *s++)) break; } while (--n);
    }
    if (!n) { if (siz) *dest = '\0'; while (*s++) {} }
    return (size_t)(s - src - 1);
}

static inline int os_snprintf_error(size_t size, int res) {
    return res < 0 || (size_t)res >= size;
}

#endif /* OS_H */
