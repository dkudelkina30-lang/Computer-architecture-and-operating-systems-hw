//-------------------------------------------------------------------------
//  общее для процессов-менеджеров: ключи, журнал, сигналы
//-------------------------------------------------------------------------
//  Сигналы у менеджера:
//    SIGINT (Ctrl+C), SIGTERM - остановить линию и вывести итоги;
//    SIGUSR1 - показать текущее состояние линии, работа продолжается;
//    SIGUSR2 - мягкая остановка: новые заготовки не выпускаются,
//              детали, которые уже в работе, доделываются.
//  Обработчик только ставит флаг, действует главный цикл менеджера.
//-------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "log.h"
#include "model.h"

volatile sig_atomic_t app_sig = 0, app_usr1 = 0, app_usr2 = 0;

static void on_signal(int sig)
{
    if (sig == SIGUSR1)
        app_usr1 = 1;
    else if (sig == SIGUSR2)
        app_usr2 = 1;
    else
        app_sig = sig;
}

void app_signals(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    int sigs[] = {SIGINT, SIGTERM, SIGUSR1, SIGUSR2};

    for (int idx = 0; idx < 4; idx++)
    {
        sigaction(sigs[idx], &sa, NULL);
    }

    signal(SIGPIPE, SIG_IGN);
}

void app_flags(void)
{
    if (app_sig && !L->stop)
    {
        L->signal_no = app_sig;
        event("СИСТЕМА", "Линия", "получен сигнал %s - остановка", app_sig == SIGINT ? "SIGINT" : "SIGTERM");
        stop_all(END_SIGNAL, app_sig == SIGINT ? "получен сигнал SIGINT (Ctrl+C)" : "получен сигнал SIGTERM");
    }
    if (app_usr1)
    {
        app_usr1 = 0;
        print_status();
    }
    if (app_usr2)
    {
        app_usr2 = 0;
        if (!L->soft_stop)
        {
            L->soft_stop = 1;
            event("СИСТЕМА", "Линия", "SIGUSR2: новые заготовки не выпускаются, доделываем детали в работе");
        }
    }
}

// Целое не меньше min из аргумента командной строки, иначе - ошибка и выход
static long arg(int opt, const char *s, long min)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);

    if (*s == '\0' || *end || errno || v < min)
    {
        dprintf(2, "Некорректное значение параметра -%c: '%s'\n", opt, s);
        exit(1);
    }

    return v;
}

int app_setup(int argc, char **argv, Options *o, Config *cfg)
{
    *o = (Options){NULL, "line.log", "asm_line", -1, 50, 0, 0, 0};
    int opt;

    while ((opt = getopt(argc, argv, "c:s:d:l:qt:n:Lh")) != -1)
    {
        switch (opt)
        {
        case 'c':
            o->cfg_path = optarg;
            break;
        case 's':
            o->seed = arg(opt, optarg, 0);
            break;
        case 'd':
            o->tick = arg(opt, optarg, 1);
            break;
        case 'l':
            o->log_path = strcmp(optarg, "-") ? optarg : NULL;
            break;
        case 'q':
            o->quiet = 1;
            break;
        case 't':
            o->max_time = arg(opt, optarg, 1);
            break;
        case 'n':
            o->name = optarg;
            break;
        case 'L':
            o->list = 1;
            break;
        case 'h':
            dprintf(1, "Использование: %s [-c файл] [-s seed] [-d мс_на_единицу] [-l журнал|-] [-q] [-t лимит]\n"
                       "       [-n имя_IPC] [-L - список процессов-участников]\n"
                       "Сигналы: SIGINT/SIGTERM - остановка, SIGUSR1 - состояние, SIGUSR2 - мягкая остановка\n",
                    argv[0]);
            return 0;
        default:
            return 1;
        }
    }

    char err[300];
    if (config_load(cfg, o->cfg_path, err, sizeof err) != 0)
    {
        dprintf(2, "Ошибка конфигурации: %s\n", err);
        return 1;
    }

    if (o->seed >= 0)
    {
        cfg->seed = o->seed;
    }
    if (cfg->seed < 0)
    {
        cfg->seed = (long)time(NULL) % 1000000;
    }
    if (o->max_time > 0)
    {
        cfg->max_time = o->max_time;
    }

    int code = config_check(cfg, err, sizeof err), raw_total = 0;
    for (int t = 0; t < cfg->n_pt; t++)
    {
        raw_total += cfg->pt[t].raw;
    }

    if (code == 0 && raw_total > MAX_PARTS)
    {
        snprintf(err, sizeof err, "заготовок всего больше %d", MAX_PARTS);
        code = 1;
    }
    if (code != 0)
    { // 1 - ошибка данных, 3 - производство невозможно
        if (!o->list)
            config_print(cfg, 1);
        dprintf(1, "%s: %s\n", code == 1 ? "Ошибка в данных" : "Производство невозможно", err);
        return code;
    }

    return -1;
}

int app_open_log(const Options *o)
{
    if (!o->log_path)
        return -1;
    int fd = open(o->log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

    if (fd < 0)
    {
        dprintf(2, "Не удалось открыть журнал '%s': %s\n", o->log_path, strerror(errno));
    }
    return fd;
}

void app_list(void)
{
    static const char *roles[] = {"warehouse", "machine", "inspector", "robot", "station"};
    for (int r = 0; r < R_COUNT; r++)
    {
        for (int idx = 0; idx < line_expected(r); idx++)
        {
            dprintf(1, "%s %d\n", roles[r], idx);
        }
    }
}
