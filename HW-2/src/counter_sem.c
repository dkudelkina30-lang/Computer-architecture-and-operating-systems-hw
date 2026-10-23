//-------------------------------------------------------------------------
//  счётчик на семафорах POSIX (основная программа line_sem)
//-------------------------------------------------------------------------

#include <errno.h>
#include "counter.h"

void counter_init(Counter *c, int value)
{
    sem_init(&c->sem, 0, (unsigned)value); // 0 - семафор общий для потоков процесса
}

void counter_wait(Counter *c)
{
    while (sem_wait(&c->sem) != 0 && errno == EINTR)
    {
        // ожидание прервано сигналом - ждём дальше
    }
}

int counter_trywait(Counter *c)
{
    return sem_trywait(&c->sem) == 0 ? 0 : -1;
}

void counter_post(Counter *c)
{
    sem_post(&c->sem);
}

void counter_destroy(Counter *c)
{
    sem_destroy(&c->sem);
}
