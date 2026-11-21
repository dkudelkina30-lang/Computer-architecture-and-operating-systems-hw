//-------------------------------------------------------------------------
//  общий приём сообщений для участника, наблюдателя и пульта
//-------------------------------------------------------------------------

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "client.h"
#include "log.h"
#include "net.h"

int client_run(const char *ip, const char *port, const char *hello, int worker)
{
    int fd = net_connect(ip, port);
    if (fd < 0)
    {
        return 1;
    }
    int code = 1;
    Input in = {0};
    long due = -1;
    if (net_write(fd, hello) < 0)
    {
        close(fd);
        dprintf(2, "Не удалось послать запрос серверу\n");
        return 1;
    }
    for (;;)
    {
        long left = due < 0 ? -1 : due - clock_ms();
        int timeout = due < 0 ? -1 : left <= 0    ? 0
                                 : left > INT_MAX ? INT_MAX
                                                  : (int)left;
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, timeout);

        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 || (r > 0 && net_read(fd, &in) < 0))
            break;

        char msg[NET_LINE];
        while ((r = net_line(&in, msg)) > 0)
        {
            if (strncmp(msg, "INFO ", 5) == 0)
            {
                dprintf(1, "%s\n", msg + 5);
            }
            else if (strncmp(msg, "STOP ", 5) == 0)
            {
                char *reason = strchr(msg + 5, ' ');

                if (reason)
                {
                    *reason++ = '\0';
                }

                code = (int)number(msg + 5, 0, 255);
                if (code >= 0)
                {
                    if (reason)
                    {
                        dprintf(1, "Завершение: %s (код %d)\n", reason, code);
                    }
                    close(fd);
                    return code;
                }
                r = -1;
                break;
            }
            else if (worker && due < 0 && strncmp(msg, "WORK ", 5) == 0)
            {
                long ms = number(msg + 5, 0, LONG_MAX - clock_ms());

                if (ms < 0)
                {
                    r = -1;
                    break;
                }
                due = clock_ms() + ms;
            }
            else
            {
                r = -1;
                break;
            }
        }
        
        if (r < 0)
        {
            break;
        }
        if (due >= 0 && clock_ms() >= due)
        {
            if (net_write(fd, "DONE\n") < 0)
                break;
            due = -1;
        }
    }
    dprintf(2, "Связь с сервером закрыта без сообщения о завершении\n");
    close(fd);
    return 1;
}
