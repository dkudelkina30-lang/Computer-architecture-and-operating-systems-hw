//-------------------------------------------------------------------------
//  пульт: список клиентов, отключение и разрешение нового подключения
//-------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "client.h"
#include "config.h"
#include "net.h"

int main(int argc, char **argv)
{
    char msg[64];
    if (argc == 4 && strcmp(argv[3], "list") == 0)
    {
        snprintf(msg, sizeof msg, "LIST\n");
    }
    else if (argc == 6 && strlen(argv[4]) < 24 && number(argv[5], 0, MAX_MACHINES - 1) >= 0 && (strcmp(argv[3], "disconnect") == 0 || strcmp(argv[3], "allow") == 0))
    {
        snprintf(msg, sizeof msg, "%s %s %ld\n", strcmp(argv[3], "disconnect") == 0 ? "DROP" : "ALLOW", argv[4], number(argv[5], 0, MAX_MACHINES - 1));
    }
    else
    {
        dprintf(2, "Использование: %s IP порт list | disconnect роль номер | allow роль номер\n", argv[0]);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }
    return client_run(argv[1], argv[2], msg, 0);
}
