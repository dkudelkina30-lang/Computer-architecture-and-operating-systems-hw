#ifndef SHM_H
#define SHM_H
//-------------------------------------------------------------------------
//  разделяемая память POSIX с состоянием линии
//-------------------------------------------------------------------------

#include <semaphore.h>
#include "model.h"

typedef struct Shared
{
    Line line;            // состояние линии
    sem_t sems[MAX_SEMS]; // место для неименованных семафоров 
} Shared;

Shared *shm_create(const char *name); // создать (менеджер / родитель)
Shared *shm_attach(const char *name); // подключиться (рабочий процесс)
void shm_remove(const char *name, Shared *s);

#endif
