//-------------------------------------------------------------------------
//  независимо запускаемый участник сборочной линии
//-------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "client.h"
#include "config.h"
#include "net.h"

int main(int argc, char **argv)
{
    const char *roles[] = {"warehouse", "machine", "inspector", "robot", "station"};
    int role = -1;

    if (argc == 5)
    {
        for (int idx = 0; idx < (int)(sizeof roles / sizeof roles[0]); idx++)
        {
            if (strcmp(argv[3], roles[idx]) == 0)
            {
                role = idx;
            }
        }
    }

    if (role < 0 || number(argv[4], 0, MAX_MACHINES - 1) < 0)
    {
        dprintf(2, "Использование: %s IP порт <warehouse|machine|inspector|robot|station> номер\n", argv[0]);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }
    
    char msg[64];
    snprintf(msg, sizeof msg, "WORKER %s %ld\n", roles[role], number(argv[4], 0, MAX_MACHINES - 1));
    return client_run(argv[1], argv[2], msg, 1);
}
