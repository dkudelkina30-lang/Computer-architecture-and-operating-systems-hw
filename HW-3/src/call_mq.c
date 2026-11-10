//-------------------------------------------------------------------------
//  операции над линией через очереди сообщений POSIX
//-------------------------------------------------------------------------
//  Состояние линии есть только у процесса-менеджера. Рабочий процесс
//  отправляет запрос в очередь /<имя>_req и ждёт ответ в своей очереди
//  /<имя>_r<номер>. Менеджер выполняет запросы строго по одному, поэтому
//  замок не нужен. Ждут процессы по-прежнему на именованных семафорах
//-------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mq.h"
#include "sync.h"
#include "worker.h"

static mqd_t req = (mqd_t)-1; // очередь запросов менеджеру
static mqd_t rep = (mqd_t)-1; // своя очередь ответов
static char rep_name[96];
static int my_slot; // роль*1000 + номер

void mq_req_name(char *buf, const char *name) { snprintf(buf, 96, "/%s_req", name); }
void mq_rep_name(char *buf, const char *name, int slot) { snprintf(buf, 96, "/%s_r%d", name, slot); }

int mq_worker_open(const char *name, int slot)
{
    char nm[96];
    my_slot = slot;
    mq_req_name(nm, name);
    int said = 0;

    while ((req = mq_open(nm, O_WRONLY)) == (mqd_t)-1)
    { // ждём, пока менеджер создаст очередь
        if (errno != ENOENT || sync_quit)
        {
            perror(nm);
            return -1;
        }
        if (!said++)
        {
            dprintf(2, "Ожидание менеджера (%s)...\n", nm);
        }
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);
    }

    struct mq_attr a = {0};
    a.mq_maxmsg = 4;
    a.mq_msgsize = sizeof(Reply);
    mq_rep_name(rep_name, name, slot);
    rep = mq_open(rep_name, O_CREAT | O_EXCL | O_RDONLY, 0600, &a);

    if (rep == (mqd_t)-1)
    {
        if (errno == EEXIST)
        {
            dprintf(2, "Очередь %s уже есть: такой участник уже запущен\n", rep_name);
        }
        else
        {
            perror(rep_name);
        }
        rep_name[0] = '\0'; // чужую очередь не удаляем
        return -1;
    }

    return 0;
}

void mq_worker_close(void)
{
    if (req != (mqd_t)-1)
        mq_close(req);
    if (rep != (mqd_t)-1)
        mq_close(rep);
    if (rep_name[0])
        mq_unlink(rep_name);
}

int call(int op, int who, int out[4])
{
    Request q = {op, who, my_slot};
    Reply a;
    
    while (mq_send(req, (const char *)&q, sizeof q, 0) != 0)
    {
        if (errno != EINTR)
        {
            return -1;
        }
    }
    while (mq_receive(rep, (char *)&a, sizeof a, NULL) < 0)
    {
        if (errno != EINTR)
        {
            return -1;
        }
    }
    if (out)
        memcpy(out, a.out, sizeof a.out);

    return a.r;
}
