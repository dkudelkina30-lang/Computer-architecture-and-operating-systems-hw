//-------------------------------------------------------------------------
//  именованные семафоры POSIX
//-------------------------------------------------------------------------
//  Семафор номер i называется /<имя>_s<i>. Создаёт их процесс-менеджер
//  (sem_open с O_CREAT), остальные процессы открывают по имени при первом
//  обращении. Удаляет - sem_unlink у менеджера при любом завершении.
//-------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include "model.h"
#include "sync.h"

volatile int sync_quit = 0;
static char prefix[64];       // имя набора семафоров
static sem_t *sems[MAX_SEMS]; // открытые семафоры (NULL - ещё не открыт)

static void sem_name(char *buf, int i) { snprintf(buf, 96, "/%s_s%d", prefix, i); }

// Семафор по номеру: открываем при первом обращении
static sem_t *get(int id)
{
    if (!sems[id])
    {
        char nm[96];
        sem_name(nm, id);
        sem_t *s = sem_open(nm, 0);
        if (s == SEM_FAILED)
        { // менеджер уже удалил семафоры
            dprintf(2, "Семафор %s недоступен: менеджер не запущен или уже завершился\n", nm);
            sync_quit = 1;
            return NULL;
        }
        sems[id] = s;
    }
    return sems[id];
}

int sync_create(const char *name, int n, const int *init, sem_t *mem)
{
    (void)mem;
    snprintf(prefix, sizeof prefix, "%s", name);
    char nm[96];

    for (int i = 0; i < n; i++)
    {
        sem_name(nm, i);
        sem_unlink(nm); // остаток прошлого аварийного запуска
        sems[i] = sem_open(nm, O_CREAT | O_EXCL, 0600, (unsigned)init[i]);
        if (sems[i] == SEM_FAILED)
        {
            perror(nm);
            sems[i] = NULL;
            return -1;
        }
    }

    return 0;
}

int sync_attach(const char *name, sem_t *mem)
{
    (void)mem;
    snprintf(prefix, sizeof prefix, "%s", name);
    return 0;
}

void sync_remove(const char *name, int n)
{
    char nm[96];
    snprintf(prefix, sizeof prefix, "%s", name);

    for (int i = 0; i < n; i++)
    {
        if (sems[i])
            sem_close(sems[i]);
        sems[i] = NULL;
        sem_name(nm, i);
        sem_unlink(nm);
    }
}

int sync_wait(int id)
{
    sem_t *s = get(id);

    if (!s)
        return -2;
    while (sem_wait(s) != 0)
    {
        if (errno != EINTR || sync_quit)
            return -1; // прервал сигнал «завершиться»
    }
    
    return 0;
}

int sync_trywait(int id)
{
    sem_t *s = get(id);
    return s && sem_trywait(s) == 0 ? 0 : -1;
}

void sync_post(int id)
{
    sem_t *s = get(id);
    if (s)
        sem_post(s);
}
