//-------------------------------------------------------------------------
//  процесс-менеджер линии на очередях сообщений POSIX
//-------------------------------------------------------------------------
//  Состояние линии хранится только здесь. Участники (программа mq_worker)
//  запускаются независимо и присылают запросы в очередь /<имя>_req;
//  менеджер выполняет их строго по одному и отвечает в очередь участника.
//  Ожидание по-прежнему идёт на именованных семафорах POSIX: участник
//  ждёт на семафоре, а менеджер увеличивает его, когда меняет линию.
//-------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "log.h"
#include "model.h"
#include "mq.h"
#include "sync.h"

static Line line;                            // линия - только у менеджера
static mqd_t replies[R_COUNT][MAX_MACHINES]; // очереди ответов участникам
static const char *ipc_name;

// Ответить участнику (очередь открываем при первом ответе)
static void answer(int slot, const Reply *a)
{
    int role = slot / 1000, i = slot % 1000;

    if (role < 0 || role >= R_COUNT || i < 0 || i >= MAX_MACHINES)
    {
        return;
    }
    if (replies[role][i] == (mqd_t)-1)
    {
        char nm[96];
        mq_rep_name(nm, ipc_name, slot);
        replies[role][i] = mq_open(nm, O_WRONLY);
        if (replies[role][i] == (mqd_t)-1)
        {
            perror(nm);
            return;
        }
    }
    while (mq_send(replies[role][i], (const char *)a, sizeof *a, 0) != 0 && errno == EINTR)
    {
    }
}

int main(int argc, char **argv)
{
    Options o;
    static Config cfg;
    int code = app_setup(argc, argv, &o, &cfg);

    if (code >= 0)
    {
        return code;
    }

    ipc_name = o.name;
    line_init(&line, &cfg, (int)o.tick, o.quiet, o.log_path);

    if (o.list)
    {
        app_list();
        return 0;
    }
    for (int r = 0; r < R_COUNT; r++)
    {
        for (int i = 0; i < MAX_MACHINES; i++)
        {
            replies[r][i] = (mqd_t)-1;
        }
    }

    // Именованные семафоры и очередь запросов
    if (sync_create(o.name, L->n_sems, L->sem_init, NULL) != 0)
    {
        sync_remove(o.name, L->n_sems);
        return 1;
    }
    char req_name[96], nm[96];
    for (int r = 0; r < R_COUNT; r++)
    { // остатки прошлого аварийного запуска
        for (int i = 0; i < line_expected(r); i++)
        {
            mq_rep_name(nm, o.name, r * 1000 + i);
            mq_unlink(nm);
        }
    }

    mq_req_name(req_name, o.name);
    mq_unlink(req_name);
    struct mq_attr attr = {0};
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Request);
    mqd_t req = mq_open(req_name, O_CREAT | O_RDONLY, 0600, &attr);

    if (req == (mqd_t)-1)
    {
        perror(req_name);
        sync_remove(o.name, L->n_sems);
        return 1;
    }

    int log_fd = app_open_log(&o);
    log_init(log_fd, o.quiet, (int)o.tick, &L->start_clock, &L->last_event_ms, &L->events);

    if (!o.quiet)
        config_print(&cfg, 1);
    if (log_fd >= 0)
        config_print(&cfg, log_fd);

    app_signals();
    say("Менеджер %s запущен. Ожидаются процессы-участники (%d): ./bin/mq_worker -n %s <роль> <номер>\n",
        o.name, L->expected, o.name);

    // Главный цикл: запросы участников + сторож
    long stop_at = 0;
    for (;;)
    {
        struct timespec dl; // ждём запрос не дольше единицы времени
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += o.tick * 1000000L;
        dl.tv_sec += dl.tv_nsec / 1000000000L;
        dl.tv_nsec %= 1000000000L;
        Request q;

        if (mq_timedreceive(req, (char *)&q, sizeof q, NULL, &dl) == (ssize_t)sizeof q)
        {
            Reply a = {0, {0, 0, 0, 0}};
            a.r = line_op(q.op, q.who, a.out); // выполняем операцию
            answer(q.slot, &a);
            if (q.op == OP_BYE || (q.op == OP_HELLO && a.r < 0))
            { // участник ушёл - закрываем его очередь
                int role = q.slot / 1000, i = q.slot % 1000;
                if (role >= 0 && role < R_COUNT && i >= 0 && i < MAX_MACHINES && replies[role][i] != (mqd_t)-1)
                {
                    mq_close(replies[role][i]);
                    replies[role][i] = (mqd_t)-1;
                }
            }
        }

        app_flags();
        if (!L->started && !L->stop && L->alive == L->expected)
        {
            line_start();
        }
        watchdog();
        if (L->stop)
        {
            if (!stop_at)
                stop_at = clock_ms();
            if (L->alive == 0)
                break; // все участники отключились
            if (clock_ms() - stop_at > 5000)
            {
                say("Не завершились процессов-участников: %d\n", L->alive);
                break;
            }
        }
    }

    check_final();
    report();
    code = exit_code();
    mq_close(req); // удаляем очередь и семафоры
    mq_unlink(req_name);
    sync_remove(o.name, L->n_sems);
    if (log_fd >= 0)
        close(log_fd);
    return code;
}
