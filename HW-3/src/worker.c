//-------------------------------------------------------------------------
// поведение процессов-участников (общее для всех программ)
//-------------------------------------------------------------------------
//  Каждый участник работает по циклу: дождаться на семафоре, что для него
//  есть работа, выполнить операцию над линией (call), «поработать» -
//  поспать столько, сколько длится операция, - и сообщить о результате.
//  Пока процесс ждёт или работает, он не держит замок, и остальные
//  процессы работают параллельно с ним
//-------------------------------------------------------------------------

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "model.h"
#include "sync.h"
#include "worker.h"

static int tick_ms = 50; // мс в единице модельного времени

static void on_term(int sig)
{
    (void)sig;
    sync_quit = 1;
}

void worker_signals(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_term; // без SA_RESTART: sem_wait прервётся
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGINT, SIG_IGN); // Ctrl+C обрабатывает менеджер / родитель
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
}

// Поработать units единиц модельного времени. -1 - велели завершиться
static int work(int units)
{
    long ms = (long)units * tick_ms;
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0)
    {
        if (errno != EINTR || sync_quit)
        {
            return -1;
        }
    }
    return 0;
}

static void warehouse(int wake, int interval)
{
    for (;;)
    {
        if (work(interval) < 0)
            return;
        int r = call(OP_W_RELEASE, 0, NULL);
        if (r < 0)
            return; // пора завершаться
        if (r == 0 && sync_wait(wake) < 0)
            return; // выпускать нечего - ждём списания
    }
}

static void machine(int i, int ready, int out_free)
{
    for (;;)
    {
        if (sync_wait(ready) < 0)
            return; // ждём деталь в очереди
        int dur = call(OP_M_TAKE, i, NULL);
        if (dur < 0)
            return;
        if (dur == 0)
            continue; // деталь успел взять другой станок
        if (work(dur) < 0)
            return; // выполняем операцию
        int r = call(OP_M_DONE, i, NULL);
        if (r < 0)
            return;
        if (r == 0)
        { // выход занят - станок приостановлен
            if (sync_wait(out_free) < 0)
                return;
            if (call(OP_M_RESUME, i, NULL) < 0)
                return;
        }
    }
}

static void inspector(int i, int ready, int out_free, int dur)
{
    for (;;)
    {
        if (sync_wait(ready) < 0)
            return; // ждём деталь на входе ОТК
        int r = call(OP_I_TAKE, i, NULL);
        if (r < 0)
            return;
        if (r == 0)
            continue;
        if (work(dur) < 0)
            return; // проверяем
        r = call(OP_I_DECIDE, i, NULL);
        if (r < 0)
            return;
        if (r == 2)
        { // выход ОТК занят - ждём места
            if (sync_wait(out_free) < 0)
                return;
            if (call(OP_I_PUT, i, NULL) < 0)
                return;
        }
    }
}

static void robot(int i, int wake, int dur)
{
    for (;;)
    {
        if (sync_wait(wake) < 0)
            return; // ждём, пока что-то изменится
        int r = call(OP_R_TAKE, i, NULL);
        if (r < 0)
            return;
        if (r == 0)
            continue; // работы нет
        work(dur);    // везём (доставить нужно в любом случае)
        call(OP_R_DELIVER, i, NULL);
    }
}

static void station(int i, int wake, int dur)
{
    for (;;)
    {
        if (sync_wait(wake) < 0)
            return; // ждём изменений в комплектах
        int r = call(OP_S_TAKE, i, NULL);
        if (r < 0)
            return;
        if (r == 0)
            continue; // полного комплекта нет
        work(dur);    // собираем
        call(OP_S_DONE, i, NULL);
    }
}

int worker_run(int role, int idx)
{
    int p[4];
    int who = role * 1000 + idx;
    int r = call(OP_HELLO, who, p);
    if (r < 0 && sync_quit)
        return 1; // менеджера нет - сообщение уже выведено
    if (r == -1)
    {
        dprintf(2, "Нет участника: %s %d\n", role_name(role), idx);
        return 1;
    }
    if (r == -2)
    {
        dprintf(2, "Участник %s %d уже подключён\n", role_name(role), idx);
        return 1;
    }
    if (r < 0)
    {
        dprintf(2, "Линия уже запущена или остановлена - подключиться нельзя\n");
        return 1;
    }
    tick_ms = p[3];
    if (sync_wait(S_START) == 0)
    { // ждём, пока подключатся все
        switch (role)
        {
        case R_WAREHOUSE:
            warehouse(p[0], p[1]);
            break;
        case R_MACHINE:
            machine(idx, p[0], p[1]);
            break;
        case R_INSPECTOR:
            inspector(idx, p[0], p[1], p[2]);
            break;
        case R_ROBOT:
            robot(idx, p[0], p[2]);
            break;
        case R_STATION:
            station(idx, p[0], p[2]);
            break;
        }
    }
    call(OP_BYE, who, p);
    return 0;
}
