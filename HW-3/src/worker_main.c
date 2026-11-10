//-------------------------------------------------------------------------
//  независимо запускаемый процесс-участник линии
//-------------------------------------------------------------------------
//  Запуск:  ./bin/shm_worker [-n имя_IPC] <роль> <номер>
//           ./bin/mq_worker  [-n имя_IPC] <роль> <номер>
//  Роли: warehouse, machine, inspector, robot, station; номер - с 0.
//  Список всех нужных участников выводит менеджер с ключом -L.
//  Участник подключается к менеджеру (ждёт его, если менеджер ещё не
//  запущен), ждёт общего старта, работает и завершается вместе с линией.
//  SIGTERM - досрочно завершить этого участника.
//-------------------------------------------------------------------------

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "model.h"
#include "sync.h"
#include "worker.h"
#ifdef USE_MQ
#include "mq.h"
#else
#include "shm.h"
#endif

int main(int argc, char **argv)
{
    static const char *roles[] = {"warehouse", "machine", "inspector", "robot", "station"};
    const char *name = "asm_line";
    int opt;

    while ((opt = getopt(argc, argv, "n:")) != -1)
    {
        if (opt == 'n')
        {
            name = optarg;
        }
        else
        {
            return 1;
        }
    }
    if (argc - optind != 2)
    {
        dprintf(2, "Использование: %s [-n имя_IPC] <warehouse|machine|inspector|robot|station> <номер>\n", argv[0]);
        return 1;
    }
    int role = -1;

    for (int r = 0; r < R_COUNT; r++)
    {
        if (strcmp(argv[optind], roles[r]) == 0)
        {
            role = r;
        }
    }
    char *end;
    long idx = strtol(argv[optind + 1], &end, 10);
    if (role < 0 || *end || idx < 0 || idx >= MAX_MACHINES)
    {
        dprintf(2, "Неизвестная роль или номер: %s %s\n", argv[optind], argv[optind + 1]);
        return 1;
    }
    worker_signals();
    int slot = role * 1000 + (int)idx;

#ifdef USE_MQ
    if (mq_worker_open(name, slot) != 0)
        return 1; // очереди: к менеджеру и своя
    sync_attach(name, NULL);
    int code = worker_run(role, (int)idx);
    mq_worker_close();
#else
    (void)slot;
    Shared *sh = shm_attach(name); // разделяемая память менеджера
    if (!sh)
    {
        return 1;
    }
    L = &sh->line;
    sync_attach(name, NULL);
    int fd = L->log_path[0] ? open(L->log_path, O_WRONLY | O_APPEND) : -1; // общий журнал
    log_init(fd, L->quiet, L->tick_ms, &L->start_clock, &L->last_event_ms, &L->events);
    int code = worker_run(role, (int)idx);
    if (fd >= 0)
    {
        close(fd);
    }
#endif
    return code;
}
