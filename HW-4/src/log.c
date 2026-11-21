//-------------------------------------------------------------------------
//  форматирование событий и модельное время
//-------------------------------------------------------------------------

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "log.h"

static int g_tick;     // миллисекунд в единице модельного времени
static long *g_start;  // момент начала
static long *g_events; // число событий
static void (*g_output)(const char *, int);

long clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void log_init(int tick_ms, long *start, long *events, void (*output)(const char *, int))
{
    g_tick = tick_ms;
    g_start = start;
    g_events = events;
    g_output = output;
}

long now_ms(void) { return *g_start ? clock_ms() - *g_start : 0; }
long now_t(void) { return now_ms() / g_tick; }

// Дополнить пробелами до width символов (не байт UTF-8)
static int pad(char *dst, const char *s, int width)
{
    int n = 0, chars = 0;
    
    for (int idx = 0; s[idx]; idx++)
    {
        dst[n++] = s[idx];
        if (((unsigned char)s[idx] & 0xC0) != 0x80)
        {
            chars++;
        }
    }
    for (int idx = chars; idx < width; idx++)
    {
        dst[n++] = ' ';
    }

    return n;
}

void event(const char *cat, const char *who, const char *fmt, ...)
{
    char line[1024];
    int n = snprintf(line, 32, "[t=%5ld] ", now_t());
    n += pad(line + n, cat, 10);
    line[n++] = ' ';
    n += pad(line + n, who, 9);
    line[n++] = ' ';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    (*g_events)++;
    g_output(line, 1);
}

void say(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_output(buf, 0);
}
