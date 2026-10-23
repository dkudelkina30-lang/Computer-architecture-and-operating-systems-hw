//-------------------------------------------------------------------------
//  счётчик на мьютексе и условной переменной
//-------------------------------------------------------------------------

#include "counter.h"

void counter_init(Counter *c, int value)
{
    pthread_mutex_init(&c->mutex, NULL);
    pthread_cond_init(&c->cond, NULL);
    c->value = value;
}

void counter_wait(Counter *c)
{
    pthread_mutex_lock(&c->mutex);
    while (c->value == 0) // ждём, пока появится «разрешение»;
    {
        pthread_cond_wait(&c->cond, &c->mutex); // на время ожидания мьютекс отпускается
    }

    c->value--;
    pthread_mutex_unlock(&c->mutex);
}

int counter_trywait(Counter *c)
{
    int ok = -1;
    pthread_mutex_lock(&c->mutex);

    if (c->value > 0)
    {
        c->value--;
        ok = 0;
    }
    pthread_mutex_unlock(&c->mutex);
    return ok;
}

void counter_post(Counter *c)
{
    pthread_mutex_lock(&c->mutex);
    c->value++;
    pthread_cond_signal(&c->cond); // будим один ждущий поток
    pthread_mutex_unlock(&c->mutex);
}

void counter_destroy(Counter *c)
{
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->mutex);
}
