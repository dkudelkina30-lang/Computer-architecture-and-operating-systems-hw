#ifndef APP_H
#define APP_H
//-------------------------------------------------------------------------
//  общее для процессов-менеджеров: ключи, журнал, сигналы
//-------------------------------------------------------------------------

#include <signal.h>
#include "config.h"

typedef struct Options
{
    const char *cfg_path;      // -c файл конфигурации (NULL - встроенная)
    const char *log_path;      // -l журнал (NULL - без журнала)
    const char *name;          // -n имя объектов IPC (памяти, семафоров, очередей)
    long seed, tick, max_time; // -s, -d, -t
    int quiet;                 // -q
    int list;                  // -L - только вывести список рабочих процессов
} Options;

// Разобрать ключи, прочитать и проверить параметры.
// Возвращает -1, если можно продолжать, иначе - код выхода.
int app_setup(int argc, char **argv, Options *o, Config *cfg);
int app_open_log(const Options *o); // открыть журнал (open, O_APPEND)
void app_signals(void);             // обработчики SIGINT/TERM/USR1/USR2
void app_flags(void);               // обработать пришедшие сигналы (под замком)
void app_list(void);                // вывести «роль номер» всех участников

extern volatile sig_atomic_t app_sig; // SIGINT или SIGTERM (0 - не было)

#endif
