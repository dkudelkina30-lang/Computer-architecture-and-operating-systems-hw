#ifndef CLIENT_H
#define CLIENT_H

// Отправить первый запрос и читать ответы до STOP
int client_run(const char *ip, const char *port, const char *hello, int worker);

#endif
