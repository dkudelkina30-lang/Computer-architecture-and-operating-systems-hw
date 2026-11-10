//-------------------------------------------------------------------------
//  неименованные семафоры POSIX в разделяемой памяти
//-------------------------------------------------------------------------
//  Массив sem_t лежит внутри разделяемой памяти, поэтому его видят все
//  процессы, порождённые fork(). Второй аргумент sem_init (pshared = 1)
//  разрешает использовать семафор из разных процессов.
//-------------------------------------------------------------------------

#include <errno.h>
#include <stddef.h>
#include "sync.h"

volatile int sync_quit = 0;
static sem_t *sems = NULL; // массив семафоров в разделяемой памяти

int sync_create(const char *name, int n, const int *init, sem_t *mem)
{
    (void)name;
    sems = mem;

    for (int idx = 0; idx < n; idx++)
    {
        if (sem_init(&sems[idx], 1, (unsigned)init[idx]) != 0)
        {
            return -1;
        }
    }
    return 0;
}

int sync_attach(const char *name, sem_t *mem)
{
    (void)name;
    sems = mem;
    return 0;
}

void sync_remove(const char *name, int n)
{
    (void)name;
    for (int idx = 0; sems && idx < n; idx++)
    {
        sem_destroy(&sems[idx]);
    }
}

int sync_wait(int id)
{
    while (sem_wait(&sems[id]) != 0)
    {
        if (errno != EINTR || sync_quit)
            return -1; // прервал сигнал «завершиться»
    }
    return 0;
}

int sync_trywait(int id) { return sem_trywait(&sems[id]) == 0 ? 0 : -1; }

void sync_post(int id) { sem_post(&sems[id]); }
