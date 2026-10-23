//-------------------------------------------------------------------------
//  многопоточная модель сборочной линии
//-------------------------------------------------------------------------
//  Запуск (из папки HW-2): ./bin/line_sem  [-c файл] [-s seed] [-d мс] [-l журнал] [-q] [-t время]
//                          ./bin/line_cond (те же ключи)
//  Главный поток: читает параметры, запускает потоки участников и поток
//  сигналов, затем работает сторожем: следит за лимитом времени и за тем,
//  не остановилась ли линия. В конце дожидается всех потоков и печатает итоги.
//
//  Сигналы: SIGINT (Ctrl+C), SIGTERM - остановить работу и вывести итоги;
//           SIGUSR1 - показать текущее состояние линии, работа продолжается;
//           SIGUSR2 - мягкая остановка: новые заготовки не выпускаются,
//                     детали, которые уже в работе, доделываются.
//  Коды завершения: 0 - план выполнен; 1 - ошибка данных; 2 - заготовки
//  исчерпаны; 3 - производство невозможно; 4 - лимит времени;
//  5 - нарушены правила модели; 128+номер сигнала - остановка сигналом.
//-------------------------------------------------------------------------

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

static sigset_t signals; // сигналы, которые обрабатывает отдельный поток

// Поток сигналов: ждёт сигнал через sigwait и реагирует на него
static void *signal_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        int sig;
        if (sigwait(&signals, &sig) != 0)
        {
            continue;
        }
        if (sig == SIGUSR1)
        { // показать состояние
            print_status();
        }
        else if (sig == SIGUSR2)
        { // мягкая остановка
            pthread_mutex_lock(&line.lock);
            line.soft_stop = 1;
            event("СИСТЕМА", "Линия", "SIGUSR2: новые заготовки не выпускаются, доделываем детали в работе");
            pthread_mutex_unlock(&line.lock);
        }
        else
        {
            pthread_mutex_lock(&line.lock);
            line.signal_no = sig;
            pthread_mutex_unlock(&line.lock);
            stop_all(END_SIGNAL, sig == SIGINT ? "получен сигнал SIGINT (Ctrl+C)" : "получен сигнал SIGTERM");
            return NULL;
        }
    }
}

static double pct(long part, long total) { return total > 0 ? 100.0 * part / total : 0.0; }

// Итоговая статистика
static void report(long T, long tick)
{
    const Config *c = line.cfg;
    static const char *reason[] = {"не определено", "ПЛАН ВЫПОЛНЕН", "ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ",
                                   "ПРОИЗВОДСТВО НЕВОЗМОЖНО (взаимная блокировка)", "ПРОИЗВОДСТВО НЕВОЗМОЖНО",
                                   "ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ", "ПРЕРВАНО СИГНАЛОМ", "МЯГКАЯ ОСТАНОВКА (SIGUSR2)"};
    say("\n========== ИТОГИ МОДЕЛИРОВАНИЯ ==========\n");
    say("Завершение: %s\nПричина: %s\n", reason[line.end], line.why);
    say("Время работы: %ld ед. (%.1f с), потоков участников: %d\n", T / tick,
        T / 1000.0, 1 + line.n_mach + c->inspectors + c->robots + c->stations);
    say("Событий: %ld, seed: %ld\n", events_count(), c->seed);
    say("Проверок правил: %ld, нарушений правил модели: %d\n", line.checks, line.violations);
    int total = 0;

    for (int p = 0; p < c->n_pr; p++)
    {
        say("Изделие %s: выпущено %d из %d\n", c->pr[p].name, line.produced[p], c->pr[p].plan);
        total += line.produced[p];
    }
    if (total)
    {
        say("Средний цикл изделия: %.1f, такт выпуска: %.1f\n", (double)line.cycle_sum / total, (double)(T / tick) / total);
    }

    for (int t = 0; t < c->n_pt; t++)
    {
        say("Деталь %s: создано %d, брак %d, переделок %d, списано %d, в изделиях %d, на линии %d, "
            "ср. путь до комплекта %.1f\n",
            c->pt[t].name, line.created[t], line.defects[t], line.reworks[t],
            line.scrapped[t], line.consumed[t], line.on_line[t], line.lead_n[t] ? (double)line.lead_sum[t] / line.lead_n[t] : 0.0);
    }
    for (int i = 0; i < line.n_mach; i++)
    {
        say("Станок %s: операций %d, работа %.1f%%, приостановлен %.1f%%\n", line.mach[i].name,
            line.mach[i].ops, pct(line.mach[i].busy_ms, T), pct(line.mach[i].blocked_ms, T));
    }
    for (int i = 0; i < c->inspectors; i++)
    {
        say("Контролёр %s: проверено %d, брак %d, работа %.1f%%, ожидание места %.1f%%\n", line.insp[i].name,
            line.insp[i].checked, line.insp[i].defects, pct(line.insp[i].busy_ms, T), pct(line.insp[i].blocked_ms, T));
    }
    for (int i = 0; i < c->robots; i++)
    {
        say("Робот %s: рейсов %d, работа %.1f%%\n", line.rob[i].name, line.rob[i].trips, pct(line.rob[i].busy_ms, T));
    }
    for (int i = 0; i < c->stations; i++)
    {
        say("Пост %s: собрано %d, работа %.1f%%\n", line.st[i].name, line.st[i].built, pct(line.st[i].busy_ms, T));
    }

    say("Максимальное заполнение: склад %d, вход ОТК %d/%d, выход ОТК %d/%d\n",
        line.warehouse.max, line.qc_in.max, line.qc_in.cap, line.qc_out.max, line.qc_out.cap);
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
    const char *cfg_path = NULL, *log_path = "line.log";
    long seed = -1, tick = 50, max_time = 0;
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
            tick = arg(opt, optarg, 1);
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
            dprintf(1, "Использование: %s [-c файл] [-s seed] [-d мс_на_единицу] [-l журнал|-] [-q] [-t лимит]\n"
                       "Сигналы: SIGINT/SIGTERM - остановка, SIGUSR1 - состояние, SIGUSR2 - мягкая остановка\n",
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
    {
        cfg.seed = seed;
    }
    if (cfg.seed < 0)
    {
        cfg.seed = (long)time(NULL) % 1000000;
    }
    if (max_time > 0)
    {
        cfg.max_time = max_time;
    }
    int raw_total = 0;
    for (int t = 0; t < cfg.n_pt; t++)
    {
        raw_total += cfg.pt[t].raw;
    }

    int log_fd = -1;
    if (log_path && (log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
    {
        dprintf(2, "Не удалось открыть журнал '%s': %s\n", log_path, strerror(errno));
    }
    log_init(log_fd, quiet, (int)tick);
    if (!quiet)
    {
        config_print(&cfg, 1);
    }
    if (log_fd >= 0)
    {
        config_print(&cfg, log_fd);
    }

    int code = config_check(&cfg, err, sizeof err);
    if (code == 0 && raw_total > MAX_PARTS)
    {
        snprintf(err, sizeof err, "заготовок всего больше %d", MAX_PARTS);
        code = 1;
    }
    if (code != 0)
    { // 1 - ошибка данных, 3 - производство невозможно
        say("%s: %s\n", code == 1 ? "Ошибка в данных" : "Производство невозможно", err);
        if (log_fd >= 0)
        {
            close(log_fd);
        }
        return code;
    }

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    sigaddset(&signals, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);

    // Запуск потоков
    line_init(&cfg, (int)tick);
    event("СИСТЕМА", "Линия", "запуск: станков %d, контролёров %d, роботов %d, постов %d - %d потоков",
          line.n_mach, cfg.inspectors, cfg.robots, cfg.stations,
          1 + line.n_mach + cfg.inspectors + cfg.robots + cfg.stations);
    pthread_t sig_tid, wh_tid;
    pthread_create(&sig_tid, NULL, signal_thread, NULL);

    for (int idx = 0; idx < line.n_mach; idx++)
    {
        pthread_create(&line.mach[idx].tid, NULL, machine_thread, &line.mach[idx]);
    }
    for (int idx = 0; idx < cfg.inspectors; idx++)
    {
        pthread_create(&line.insp[idx].tid, NULL, inspector_thread, &line.insp[idx]);
    }
    for (int idx = 0; idx < cfg.robots; idx++)
    {
        pthread_create(&line.rob[idx].tid, NULL, robot_thread, &line.rob[idx]);
    }
    for (int idx = 0; idx < cfg.stations; idx++)
    {
        pthread_create(&line.st[idx].tid, NULL, station_thread, &line.st[idx]);
    }
    pthread_create(&wh_tid, NULL, warehouse_thread, NULL);

    int longest = cfg.inspect_time + cfg.transport_time + cfg.assembly_time + cfg.release_interval;
    for (int t = 0; t < cfg.n_pt; t++)
    {
        for (int s = 0; s < cfg.pt[t].route_len; s++)
        {
            if (cfg.pt[t].route[s].duration > longest)
            {
                longest = cfg.pt[t].route[s].duration;
            }
        }
    }
    long quiet_ms = (2L * longest + 5) * tick;
    struct timespec nap = {tick / 1000, (tick % 1000) * 1000000L};

    while (!stopping())
    {
        nanosleep(&nap, NULL);
        if (now_t() > cfg.max_time)
        {
            stop_all(END_TIMEOUT, "достигнут лимит модельного времени");
        }
        else if (now_ms() - last_event_ms() > quiet_ms)
        {
            char why[256];
            pthread_mutex_lock(&line.lock);
            if (line.soft_stop)
            {
                line.end = END_SOFT;
                snprintf(line.why, sizeof line.why, "детали, которые были в работе, доделаны");
            }
            else
            {
                diagnose(); // заполняет line.end и line.why
            }
            int reason = line.end;
            snprintf(why, sizeof why, "%s", line.why);
            pthread_mutex_unlock(&line.lock);
            stop_all(reason, why);
        }
    }

    // Завершение: ждём все потоки
    for (int i = 0; i < line.n_mach; i++)
    {
        pthread_join(line.mach[i].tid, NULL);
    }
    for (int i = 0; i < cfg.inspectors; i++)
    {
        pthread_join(line.insp[i].tid, NULL);
    }
    for (int i = 0; i < cfg.robots; i++)
    {
        pthread_join(line.rob[i].tid, NULL);
    }
    for (int i = 0; i < cfg.stations; i++)
    {
        pthread_join(line.st[i].tid, NULL);
    }

    pthread_join(wh_tid, NULL);
    pthread_cancel(sig_tid); // поток сигналов ждёт в sigwait
    pthread_join(sig_tid, NULL);

    long T = now_ms();
    check_final();
    report(T, tick);
    if (log_fd >= 0)
    {
        close(log_fd);
    }

    static const int codes[] = {3, 0, 2, 3, 3, 4, 0, 0};
    code = codes[line.end];
    if (line.end == END_SIGNAL)
    {
        code = 128 + line.signal_no;
    }
    if (line.end == END_SOFT)
    {
        code = 128 + SIGUSR2;
    }
    if (code == 0 && line.violations)
    {
        code = 5;
    }

    line_destroy();
    return code;
}
