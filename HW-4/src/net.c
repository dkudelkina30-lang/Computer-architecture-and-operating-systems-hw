//-------------------------------------------------------------------------
//  адрес, соединение и чтение строк TCP
//-------------------------------------------------------------------------

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "net.h"

long number(const char *s, long min, long max)
{
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    return *s && !*end && !errno && n >= min && n <= max ? n : -1;
}

int net_addr(struct sockaddr_in *addr, const char *ip, const char *port)
{
    long n = number(port, 1, 65535);
    *addr = (struct sockaddr_in){0};
    addr->sin_family = AF_INET;

    if (n < 0 || inet_pton(AF_INET, ip, &addr->sin_addr) != 1)
    {
        dprintf(2, "Некорректный IPv4-адрес или порт: %s %s\n", ip, port);
        return -1;
    }

    addr->sin_port = htons((unsigned short)n);
    return 0;
}

int net_connect(const char *ip, const char *port)
{
    struct sockaddr_in addr;

    if (net_addr(&addr, ip, port) < 0)
    {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    return fd;
}

int net_read(int fd, Input *in)
{
    if (in->size == (int)sizeof in->data)
    {
        return -1;
    }

    ssize_t n = recv(fd, in->data + in->size, sizeof in->data - (size_t)in->size, 0);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    {
        return 0;
    }
    if (n <= 0)
    {
        return -1;
    }

    in->size += (int)n;
    return 1;
}

int net_line(Input *in, char *line)
{
    char *end = memchr(in->data, '\n', (size_t)in->size);
    if (!end)
    {
        return in->size == (int)sizeof in->data ? -1 : 0;
    }

    int n = (int)(end - in->data);
    if (memchr(in->data, '\0', (size_t)n))
    {
        return -1;
    }

    memcpy(line, in->data, (size_t)n);
    line[n] = '\0';
    in->size -= n + 1;
    memmove(in->data, end + 1, (size_t)in->size);
    return 1;
}

int net_write(int fd, const char *text)
{
    size_t size = strlen(text);
    for (size_t idx = 0; idx < size;)
    {
        ssize_t n = send(fd, text + idx, size - idx, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            return -1;
        }
        
        idx += (size_t)n;
    }
    return 0;
}
