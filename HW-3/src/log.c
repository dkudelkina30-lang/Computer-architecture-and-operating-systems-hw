//-------------------------------------------------------------------------
//  вывод событий системным вызовом write()
//-------------------------------------------------------------------------
//  Печатать могут сразу несколько процессов. Каждая строка выводится одним
//  вызовом write(), а журнал открыт с флагом O_APPEND, поэтому строки
//  разных процессов не перемешиваются. События печатаются под замком
//  линии, поэтому и порядок строк совпадает с порядком событий.
//-------------------------------------------------------------------------

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "log.h"

static int g_log = -1;        // дескриптор журнала (-1 - журнала нет)
static int g_quiet = 0;       // тихий режим: события только в журнал
static int g_tick = 50;       // сколько миллисекунд длится единица модельного времени
static long *g_start = NULL;  // момент начала, мс (в общей памяти)
static long *g_last = NULL;   // время последнего события (в общей памяти)
static long *g_events = NULL; // число событий (в общей памяти)

long clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void log_init(int log_fd, int quiet, int tick_ms, long *start, long *last, long *events)
{
    g_log = log_fd;
    g_quiet = quiet;
    g_tick = tick_ms > 0 ? tick_ms : 1;
    g_start = start;
    g_last = last;
    g_events = events;
}

// До старта моделирования (момент начала ещё 0) время считается нулевым
long now_ms(void) { return g_start && *g_start ? clock_ms() - *g_start : 0; }
long now_t(void) { return now_ms() / g_tick; }

// Записать len байт целиком (write может записать часть)
static void write_all(int fd, const char *s, int len)
{
    while (fd >= 0 && len > 0)
    {
        ssize_t n = write(fd, s, (size_t)len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return;
        s += n;
        len -= (int)n;
    }
}

// Дописать строку и дополнить пробелами до width символов
static int pad(char *dst, const char *s, int width)
{
    int n = 0, chars = 0;
    for (; s[n]; n++)
    {
        dst[n] = s[n];
        if (((unsigned char)s[n] & 0xC0) != 0x80)
        {
            chars++;
        }
    }
    for (; chars < width; chars++)
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
    int room = (int)sizeof line - n - 1;
    int m = vsnprintf(line + n, (size_t)room, fmt, ap);
    va_end(ap);
    n += (m < room) ? m : room - 1;
    line[n++] = '\n';

    if (g_last)
    {
        *g_last = now_ms();
    }
    if (g_events)
    {
        (*g_events)++;
    }
    write_all(g_log, line, n);
    if (!g_quiet)
    {
        write_all(1, line, n);
    }
}

void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf)
    {
        n = (int)sizeof buf - 1;
    }
    write_all(1, buf, n);
    write_all(g_log, buf, n);
}
