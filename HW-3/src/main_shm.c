//-------------------------------------------------------------------------
//  процесс-менеджер линии на разделяемой памяти POSIX
//-------------------------------------------------------------------------

//  Из этого файла собираются три программы:
//    line_unnamed - родитель порождает участников через fork(),
//                   неименованные семафоры лежат в разделяемой памяти
//    line_named   - то же, но семафоры именованные (sem_open)
//    shm_manager  - участники запускаются отдельно как программы shm_worker,
//                   семафоры именованные (собирается с -DINDEPENDENT)
//
//  Менеджер создаёт разделяемую память и семафоры, ждёт подключения всех
//  участников, затем работает «сторожем»: раз в единицу времени проверяет
//  сигналы, лимит времени и не встала ли линия. После остановки ждёт
//  завершения участников, выводит итоги и удаляет память и семафоры
//-------------------------------------------------------------------------

#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "log.h"
#include "shm.h"
#include "sync.h"
#include "worker.h"

static void lock(void)
{
    while (sync_wait(S_LOCK) != 0)
    {
    }
}
static void unlock(void) { sync_post(S_LOCK); }

static void nap(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    Options o;
    static Config cfg;
    int code = app_setup(argc, argv, &o, &cfg);

    if (code >= 0)
        return code;

    if (o.list)
    { // только список участников, объекты IPC не трогаем
        static Line tmp;
        line_init(&tmp, &cfg, (int)o.tick, o.quiet, o.log_path);
        app_list();
        return 0;
    }

    Shared *sh = shm_create(o.name);
    if (!sh)
    {
        return 1;
    }
    line_init(&sh->line, &cfg, (int)o.tick, o.quiet, o.log_path);

    if (sync_create(o.name, L->n_sems, L->sem_init, sh->sems) != 0)
    {
        sync_remove(o.name, L->n_sems);
        shm_remove(o.name, sh);
        return 1;
    }

    int log_fd = app_open_log(&o);
    log_init(log_fd, o.quiet, (int)o.tick, &L->start_clock, &L->last_event_ms, &L->events);

    if (!o.quiet)
    {
        config_print(&cfg, 1);
    }
    if (log_fd >= 0)
    {
        config_print(&cfg, log_fd);
    }
    app_signals();

#ifndef INDEPENDENT
    pid_t pids[1 + MAX_MACHINES + 3 * MAX_ACTORS];
    int n = 0;
    for (int r = 0; r < R_COUNT; r++)
    {
        for (int i = 0; i < line_expected(r); i++)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                worker_signals();
                _exit(worker_run(r, i));
            }
            if (pid > 0)
            {
                pids[n++] = pid;
            }
        }
    }
#else
    say("Менеджер %s запущен. Ожидаются процессы-участники (%d): ./bin/shm_worker -n %s <роль> <номер>\n",
        o.name, L->expected, o.name);
#endif

    for (;;)
    {
        nap(o.tick);
        lock();
        app_flags();

        if (!L->started && !L->stop && L->alive == L->expected)
        {
            line_start();
        }

        watchdog();
        int stop = L->stop;
        unlock();

        if (stop)
        {
            break;
        }
    }

#ifndef INDEPENDENT
    for (int k = 0; k < n; k++)
    {
        while (waitpid(pids[k], NULL, 0) < 0 && errno == EINTR)
        {
        }
    }
#else
    long deadline = clock_ms() + 5000;
    for (;;)
    {
        lock();
        int alive = L->alive;
        unlock();
        if (alive == 0)
        {
            break;
        }
        if (clock_ms() > deadline)
        {
            say("Не завершились процессов-участников: %d\n", alive);
            break;
        }
        nap(o.tick);
    }
#endif

    check_final();
    report();
    code = exit_code();
    int n_sems = L->n_sems;
    sync_remove(o.name, n_sems);
    shm_remove(o.name, sh);
    if (log_fd >= 0)
    {
        close(log_fd);
    }
    return code;
}
