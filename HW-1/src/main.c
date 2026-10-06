//  Консольное приложение "Моделирование сборочной линии"
//--------------------------------------------------------------------------------------
//  Запуск: ./bin/assembly_line [-c файл] [-s seed] [-d мс] [-l журнал] [-q] [-t время]
//  Коды завершения:
//    0 - план выполнен;
//    1 - ошибка параметров или данных;
//    2 - исчерпаны заготовки;
//    3 - производство невозможно;
//    4 - превышен лимит времени;
//    5 - нарушены инварианты;
//    130 / 143 - прервано сигналом SIGINT (Ctrl+C) / SIGTERM.
//---------------------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "log.h"
#include "model.h"

static volatile sig_atomic_t g_signal = 0;

// В обработчике сигнала ставим флаг.
// Завершение с выводом итогов делает главный цикл.
static void on_signal(int sig) { g_signal = sig; }

static double pct(long part, long total) { return total ? 100.0 * part / total : 0; }

// Итоговая статистика
static void report(const Factory *f)
{
    const Config *c = f->cfg;
    static const char *reason[] = {
        "не определено", "ПЛАН ВЫПОЛНЕН", "ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ",
        "ПРОИЗВОДСТВО НЕВОЗМОЖНО (взаимная блокировка)", "ПРОИЗВОДСТВО НЕВОЗМОЖНО",
        "ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ", "ПРЕРВАНО СИГНАЛОМ"};
    long T = f->now;
    int total = 0;
    say("\n========== ИТОГИ МОДЕЛИРОВАНИЯ ==========\n");
    say("Завершение: %s\nПричина: %s\n", reason[f->end], f->why);
    say("Модельное время: %ld, событий: %ld, seed: %ld\n", T, f->events, c->seed);
    say("Проверок инвариантов: %ld, нарушений инвариантов: %ld\n", f->checks, f->violations);

    for (int p = 0; p < c->n_pr; p++)
    {
        say("Изделие %s: выпущено %d из %d\n", c->pr[p].name, f->produced[p], c->pr[p].plan);
        total += f->produced[p];
    }

    if (total)
    {
        say("Средний цикл изделия: %.1f, такт выпуска: %.1f\n", (double)f->cycle_sum / total, (double)T / total);
    }

    for (int t = 0; t < c->n_pt; t++)
    {
        say("Деталь %s: создано %d, брак %d, переделок %d, списано %d, в изделиях %d, на линии %d, "
            "ср. путь до комплекта %.1f\n",
            c->pt[t].name, f->created[t], f->defects[t], f->reworks[t],
            f->scrapped[t], f->consumed[t], f->on_line[t],
            f->lead_n[t] ? (double)f->lead_sum[t] / f->lead_n[t] : 0.0);
    }

    for (int idx = 0; idx < f->n_mach; idx++)
    {
        say("Станок %s: операций %d, работа %.1f%%, приостановлен %.1f%%\n", f->mach[idx].a.name,
            f->mach[idx].ops, pct(f->mach[idx].a.busy, T), pct(f->mach[idx].a.blocked, T));
    }

    for (int idx = 0; idx < c->inspectors; idx++)
    {
        say("Контролёр %s: проверено %d, брак %d, работа %.1f%%, ожидание места %.1f%%\n",
            f->insp[idx].a.name, f->insp[idx].checked, f->insp[idx].defects,
            pct(f->insp[idx].a.busy, T), pct(f->insp[idx].a.blocked, T));
    }

    for (int idx = 0; idx < c->robots; idx++)
    {
        say("Робот %s: рейсов %d, работа %.1f%%\n", f->rob[idx].a.name, f->rob[idx].trips, pct(f->rob[idx].a.busy, T));
    }

    for (int idx = 0; idx < c->stations; idx++)
    {
        say("Пост %s: собрано %d, работа %.1f%%\n", f->st[idx].a.name, f->st[idx].built, pct(f->st[idx].a.busy, T));
    }

    say("Максимальное заполнение: склад %d, вход ОТК %d/%d, выход ОТК %d/%d\n",
        f->warehouse.max, f->qc_in.max, f->qc_in.cap, f->qc_out.max, f->qc_out.cap);
    say("=========================================\n");
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

int main(int argc, char **argv)
{
    const char *cfg_path = NULL, *log_path = "assembly_line.log";
    long seed = -1, delay = 0, max_time = 0;
    int quiet = 0, opt;

    while ((opt = getopt(argc, argv, "c:s:d:l:qt:h")) != -1)
    {
        switch (opt)
        {
        case 'c':
            cfg_path = optarg;
            break;
        case 's':
            seed = arg(opt, optarg, 0);
            break;
        case 'd':
            delay = arg(opt, optarg, 0);
            break;
        case 'l':
            log_path = strcmp(optarg, "-") ? optarg : NULL;
            break;
        case 'q':
            quiet = 1;
            break;
        case 't':
            max_time = arg(opt, optarg, 1);
            break;
        case 'h':
            dprintf(1, "Использование: %s [-c файл] [-s seed] [-d задержка_мс] [-l журнал|-] [-q] [-t лимит_времени]\n"
                       "Без -c используется встроенная конфигурация. Журнал по умолчанию: assembly_line.log\n",
                    argv[0]);
            return 0;
        default:
            return 1;
        }
    }

    static Config cfg;
    char err[300];

    if (config_load(&cfg, cfg_path, err, sizeof err) != 0)
    {
        dprintf(2, "Ошибка конфигурации: %s\n", err);
        return 1;
    }

    if (seed >= 0)
        cfg.seed = seed;
    if (cfg.seed < 0)
        cfg.seed = (long)time(NULL) % 1000000;
    if (max_time > 0)
        cfg.max_time = max_time;

    int log_fd = -1;
    if (log_path && (log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
    {
        dprintf(2, "Не удалось открыть журнал '%s': %s\n", log_path, strerror(errno));
    }

    log_init(log_fd, quiet, (int)delay);

    if (!quiet)
    {
        config_print(&cfg, 1);
    }
    if (log_fd >= 0)
    {
        config_print(&cfg, log_fd);
    }

    int code = config_check(&cfg, err, sizeof err);
    if (code != 0)
    {
        say("%s: %s\n", code == 1 ? "Ошибка в данных" : "Производство невозможно", err);

        if (log_fd >= 0)
        {
            close(log_fd);
        }

        return code;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static Factory fac;
    sim_init(&fac, &cfg);
    Event e;
    while (!g_signal)
    {
        if (!sim_next(&fac, &e))
        {
            sim_diagnose(&fac);
            break;
        }
        if (e.time > cfg.max_time)
        {
            fac.end = END_TIMEOUT;
            snprintf(fac.why, sizeof fac.why, "следующее событие позже t=%ld", cfg.max_time);
            break;
        }
        sim_handle(&fac, &e);
        if (sim_plan_done(&fac))
        {
            fac.end = END_PLAN;
            snprintf(fac.why, sizeof fac.why, "все изделия по плану выпущены");
            break;
        }
    }
    if (g_signal)
    {
        fac.end = END_SIGNAL;
        snprintf(fac.why, sizeof fac.why, "получен сигнал %s в t=%ld, итоги сохранены",
                 g_signal == SIGINT ? "SIGINT" : "SIGTERM", fac.now);
    }
    sim_finish(&fac);
    report(&fac);

    if (log_fd >= 0)
    {
        close(log_fd);
    }

    static const int codes[] = {3, 0, 2, 3, 3, 4, 0};
    code = fac.end == END_SIGNAL ? 128 + g_signal : codes[fac.end];

    if (code == 0 && fac.violations)
    {
        code = 5;
    }

    sim_free(&fac);
    return code;
}
