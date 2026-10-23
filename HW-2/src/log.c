//-------------------------------------------------------------------------
//  вывод событий системным вызовом write()
//-------------------------------------------------------------------------
//  Печатать могут сразу несколько потоков, поэтому вывод защищён мьютексом:
//  строка одного потока не перемешается со строкой другого.
//-------------------------------------------------------------------------

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "log.h"

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_log = -1;    // дескриптор журнала
static int g_quiet = 0;   // тихий режим: события только в журнал
static int g_tick = 50;   // сколько миллисекунд длится единица модельного времени
static long g_start = 0;  // момент запуска, мс
static long g_last = 0;   // момент последнего события, мс
static long g_events = 0; // сколько событий выведено

// Текущее время по монотонным часам, мс
static long clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void log_init(int log_fd, int quiet, int tick_ms)
{
    g_log = log_fd;
    g_quiet = quiet;
    g_tick = tick_ms > 0 ? tick_ms : 1;
    g_start = clock_ms();
    g_last = 0;
}

long now_ms(void) { return clock_ms() - g_start; }
long now_t(void) { return now_ms() / g_tick; }

long events_count(void)
{
    pthread_mutex_lock(&log_mutex);
    long n = g_events;
    pthread_mutex_unlock(&log_mutex);
    return n;
}

long last_event_ms(void)
{
    pthread_mutex_lock(&log_mutex);
    long t = g_last;
    pthread_mutex_unlock(&log_mutex);
    return t;
}

// Записать len байт целиком
static void write_all(int fd, const char *s, int len)
{
    while (fd >= 0 && len > 0)
    {
        ssize_t n = write(fd, s, (size_t)len);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0)
        {
            return;
        }
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

    pthread_mutex_lock(&log_mutex);
    g_last = now_ms();
    g_events++;
    write_all(g_log, line, n);

    if (!g_quiet)
    {
        write_all(1, line, n);
    }
    pthread_mutex_unlock(&log_mutex);
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
    
    pthread_mutex_lock(&log_mutex);
    write_all(1, buf, n);
    write_all(g_log, buf, n);
    pthread_mutex_unlock(&log_mutex);
}
