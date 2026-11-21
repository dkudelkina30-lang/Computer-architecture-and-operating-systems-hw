#ifndef NET_H
#define NET_H
//-------------------------------------------------------------------------
//  TCP: сообщения заканчиваются переводом строки
//-------------------------------------------------------------------------

#include <netinet/in.h>

#define NET_LINE 2048
#define MAX_WATCH 16

typedef struct Input
{
    char data[NET_LINE];
    int size; // сколько байт ещё не разобрано
} Input;

int net_addr(struct sockaddr_in *addr, const char *ip, const char *port);
int net_connect(const char *ip, const char *port);
int net_read(int fd, Input *in);      // 1 - данные, 0 - пока нет, -1 - связь закрыта/ошибка
int net_line(Input *in, char *line);  // 1 - строка, 0 - неполная, -1 - ошибка формата
int net_write(int fd, const char *text); // короткий запрос клиента, записать целиком
long number(const char *s, long min, long max);

#endif
