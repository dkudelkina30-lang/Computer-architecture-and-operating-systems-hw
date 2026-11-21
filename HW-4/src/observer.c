//-------------------------------------------------------------------------
//  наблюдатель: показывает состояние линии и общий поток событий
//-------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "client.h"
#include "net.h"

int main(int argc, char **argv)
{
    long who = argc == 4 ? number(argv[3], 0, MAX_WATCH - 1) : -1;
    
    if (who < 0)
    {
        dprintf(2, "Использование: %s IP порт номер_наблюдателя (0..%d)\n", argv[0], MAX_WATCH - 1);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }

    char msg[32];
    snprintf(msg, sizeof msg, "WATCH %ld\n", who);
    return client_run(argv[1], argv[2], msg, 0);
}
