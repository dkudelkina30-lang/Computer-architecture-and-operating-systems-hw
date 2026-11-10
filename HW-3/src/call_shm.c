//-------------------------------------------------------------------------
//  операции над линией в разделяемой памяти POSIX
//-------------------------------------------------------------------------
//  Состояние линии лежит в разделяемой памяти и доступно всем процессам.
//  Операцию процесс выполняет сам, захватив семафор-замок
//  S_LOCK (двоичный семафор, начальное значение 1).
//-------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "log.h"
#include "shm.h"
#include "sync.h"
#include "worker.h"

static void shm_name(char *buf, const char *name) { snprintf(buf, 96, "/%s_shm", name); }

Shared *shm_create(const char *name)
{
    char nm[96];
    shm_name(nm, name);
    shm_unlink(nm); // остаток прошлого аварийного запуска
    int fd = shm_open(nm, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (fd < 0)
    {
        perror(nm);
        return NULL;
    }
    if (ftruncate(fd, sizeof(Shared)) != 0)
    {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    Shared *s = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // отображение остаётся и без дескриптора
    return s == MAP_FAILED ? NULL : s;
}

// Подключиться к памяти, которую создал менеджер
Shared *shm_attach(const char *name)
{
    char nm[96];
    shm_name(nm, name);
    int fd, said = 0;

    while ((fd = shm_open(nm, O_RDWR, 0)) < 0)
    {
        if (errno != ENOENT || sync_quit)
        {
            perror(nm);
            return NULL;
        }
        if (!said++)
            dprintf(2, "Ожидание менеджера (%s)...\n", nm);
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);
    }

    struct timespec ts = {0, 100000000L}; // даём менеджеру заполнить память
    nanosleep(&ts, NULL);
    Shared *s = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return s == MAP_FAILED ? NULL : s;
}

void shm_remove(const char *name, Shared *s)
{
    char nm[96];
    shm_name(nm, name);
    if (s)
    {
        munmap(s, sizeof(Shared));
    }
    shm_unlink(nm);
}

int call(int op, int who, int out[4])
{
    int tmp[4];
    if (!out)
    {
        out = tmp;
    }

    int w;
    while ((w = sync_wait(S_LOCK)) == -1)
    {
    } // замок берём даже при завершении

    if (w == -2)
        return -1; // семафоров уже нет
        
    int r = line_op(op, who, out);
    sync_post(S_LOCK);
    return r;
}
