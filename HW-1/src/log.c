//-------------------------------------------
//  Вывод событий системным вызовом write()
//-------------------------------------------

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

static int g_log = -1;  // Дескриптор журнала
static int g_quiet = 0; // Тихий режим
static int g_delay = 0; // Задержка отображения, мс

void log_init(int log_fd, int quiet, int delay_ms)
{
    g_log = log_fd;
    g_quiet = quiet;
    g_delay = delay_ms;
}

// Записать len байт целиком: write может записать часть или прерваться сигналом
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

// Дописать строку s и дополнить пробелами до width символов.
// Символы считаются по UTF-8: байты вида 10xxxxxx - продолжение буквы.
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

void event(long t, const char *cat, const char *who, const char *fmt, ...)
{
    char line[1024];
    int n = snprintf(line, 32, "[t=%5ld] ", t);
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

    write_all(g_log, line, n);
    if (!g_quiet || cat[0] == '!')
    {
        write_all(1, line, n);
        if (g_delay > 0)
        {
            struct timespec ts = {g_delay / 1000, (g_delay % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
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
