//-------------------------------------------------------------------------
//  TCP-сервер сборочной линии
//-------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "log.h"
#include "model.h"
#include "net.h"

#define WATCH_SLOT (R_COUNT * MAX_MACHINES)
#define MAX_CONN (1 + MAX_MACHINES + 3 * MAX_ACTORS + MAX_WATCH + 16)
#define OUTPUT_SIZE 65536

enum
{
    WAIT,
    WORK,
    PUT
}; // ждёт задания, выполняет, ждёт места на выходе

typedef struct Conn
{
    int fd, slot, local; // slot = -1 до определения роли
    Input in;
    char out[OUTPUT_SIZE];
    int from, size; // ещё не отправленная часть out
    long opened, closing;
    int eof_sent, overflow;
} Conn;

typedef struct Client
{
    Conn *conn;
    int stage, banned, seen;
    long due, remaining, missing; // момент окончания, остаток работы, момент разрыва
} Client;

static Line line;
static Conn connections[MAX_CONN];
static Client clients[WATCH_SLOT + MAX_WATCH];
static int workers[1 + MAX_MACHINES + 3 * MAX_ACTORS], n_workers;
static Conn *source, *only; // локальные события участника / параметры и снимок одному наблюдателю
static int log_fd = -1, quiet;
static long wait_ms = 15000;
static volatile sig_atomic_t interrupted;
static const char *roles[] = {"warehouse", "machine", "inspector", "robot", "station", "observer"};

static void on_signal(int sig) { interrupted = sig; }

static void usage(const char *name)
{
    dprintf(1, "Использование: %s IP порт [-c файл] [-s seed] [-d мс] [-l журнал|-] [-q] [-t лимит] [-w секунды] [-L]\n", name);
}

// Строки добавляются в очередь, а не посылаются из операций над линией.
static void queue(Conn *c, const char *text)
{
    if (!c || c->overflow || c->eof_sent)
    {
        return;
    }
    int n = (int)strlen(text);

    if (n > OUTPUT_SIZE - c->size)
    {
        c->overflow = 1; // медленный клиент не должен задерживать остальных
        return;
    }
    if (c->from + c->size + n > OUTPUT_SIZE)
    {
        memmove(c->out, c->out + c->from, (size_t)c->size);
        c->from = 0;
    }

    memcpy(c->out + c->from + c->size, text, (size_t)n);
    c->size += n;
}

static void output(const char *text, int is_event)
{
    if (!only)
    {
        if (log_fd >= 0)
        {
            dprintf(log_fd, "%s%s", text, is_event ? "\n" : "");
        }
        if (!is_event || (!source && !quiet))
        {
            dprintf(1, "%s%s", text, is_event ? "\n" : "");
        }
    }

    for (const char *idx = text; *idx;)
    {
        size_t n = strcspn(idx, "\n");
        char msg[NET_LINE];
        snprintf(msg, sizeof msg, "INFO %.*s\n", (int)n, idx);

        if (only)
        {
            queue(only, msg);
        }
        else
        {
            if (source)
            {
                queue(source, msg);
            }
            for (int idx_watch = 0; idx_watch < MAX_WATCH; idx_watch++)
            {
                queue(clients[WATCH_SLOT + idx_watch].conn, msg);
            }
        }

        idx += n + (idx[n] == '\n');
    }
}

static void config_line(const char *text)
{
    if (only)
    {
        output(text, 0);
    }
    else
    {
        if (!quiet)
        {
            dprintf(1, "%s\n", text);
        }
        if (log_fd >= 0)
        {
            dprintf(log_fd, "%s\n", text);
        }
    }
}

static void detach(Conn *c)
{
    int slot = c->slot;
    if (slot < 0)
    {
        return;
    }

    Client *w = &clients[slot];
    c->slot = -1;
    w->conn = NULL;

    if (slot >= WATCH_SLOT)
    {
        if (!L->stop)
        {
            dprintf(2, "TCP: наблюдатель %d отключился\n", slot - WATCH_SLOT);
        }
        return;
    }
    L->alive--;
    if (!L->stop)
    {
        w->missing = clock_ms();
        if (w->stage == WORK)
        {
            w->remaining = w->due > w->missing ? w->due - w->missing : 0;
        }
        event("СИСТЕМА", actor_name(slot / MAX_MACHINES, slot % MAX_MACHINES),
              "участник отключился (%d из %d), состояние сохранено", L->alive, L->expected);
    }
}

static void close_conn(Conn *c)
{
    detach(c);
    close(c->fd);
    c->fd = -1;
}

static void finish(Conn *c, int code, const char *reason)
{
    char msg[NET_LINE];

    if (reason)
    {
        snprintf(msg, sizeof msg, "STOP %d %s\n", code, reason);
    }
    else
    {
        snprintf(msg, sizeof msg, "STOP %d\n", code);
    }

    queue(c, msg);
    c->closing = clock_ms() + 1000;
}

static void start_work(Client *w, long ms)
{
    char msg[64];
    w->stage = WORK;
    w->due = clock_ms() + ms;
    snprintf(msg, sizeof msg, "WORK %ld\n", ms);
    queue(w->conn, msg);
}

static int assign(int slot)
{
    static const int take_op[] = {OP_W_RELEASE, OP_M_TAKE, OP_I_TAKE, OP_R_TAKE, OP_S_TAKE};
    int role = slot / MAX_MACHINES, who = slot % MAX_MACHINES;
    Client *w = &clients[slot];

    if (!w->conn || w->stage == WORK)
        return 0;

    source = w->conn;
    int changed = 0;

    if (w->stage == PUT)
    {
        Store *out = role == R_MACHINE ? &L->mach[who].out : &L->qc_out;
        if (store_room(out))
        {
            line_op(role == R_MACHINE ? OP_M_RESUME : OP_I_PUT, who);
            w->stage = WAIT;
            changed = 1;
        }
    }

    if (w->stage == WAIT)
    {
        int r = line_op(take_op[role], who);
        if (r > 0)
        {
            int durations[] = {L->cfg.release_interval, r, L->cfg.inspect_time,
                               L->cfg.transport_time, L->cfg.assembly_time};
            start_work(w, (long)durations[role] * L->tick_ms);
            changed = 1;
        }
    }
    source = NULL;
    return changed;
}

static void complete(Conn *c)
{
    static const int done_op[] = {OP_W_RELEASE, OP_M_DONE, OP_I_DECIDE, OP_R_DELIVER, OP_S_DONE};
    int slot = c->slot, role = slot / MAX_MACHINES;
    Client *w = &clients[slot];

    if (w->stage != WORK)
    {
        detach(c);
        finish(c, 1, "Нет незавершённой работы");
        return;
    }
    source = c;
    int r = line_op(done_op[role], slot % MAX_MACHINES);
    source = NULL;
    w->stage = (role == R_MACHINE && r == 0) || (role == R_INSPECTOR && r == 2) ? PUT : WAIT;

    if (role == R_WAREHOUSE && r > 0)
    {
        start_work(w, (long)L->cfg.release_interval * L->tick_ms);
    }
}

static int client_slot(const char *name, const char *text)
{
    int who = (int)number(text, 0, MAX_MACHINES - 1);
    for (int idx = 0; who >= 0 && idx <= R_COUNT; idx++)
    {
        int count = idx == R_COUNT ? MAX_WATCH : line_expected(idx);
        if (strcmp(name, roles[idx]) == 0 && who < count)
        {
            return idx * MAX_MACHINES + who;
        }
    }
    return -1;
}

static void join(Conn *c, int slot)
{
    Client *w = &clients[slot];

    if (w->banned || w->conn)
    {
        finish(c, 1, w->banned ? "Подключение запрещено оператором" : "Такой клиент уже подключён");
        return;
    }

    int again = w->seen;
    w->seen = 1;
    w->conn = c;
    c->slot = slot;

    if (slot >= WATCH_SLOT)
    {
        dprintf(2, "TCP: наблюдатель %d %s\n", slot - WATCH_SLOT, again ? "подключился повторно" : "подключился");
        only = c;
        config_print(&L->cfg, config_line);
        if (again || L->alive || L->started)
        {
            print_status();
        }
        only = NULL;
    }
    else
    {
        L->alive++;
        source = c;
        event("СИСТЕМА", actor_name(slot / MAX_MACHINES, slot % MAX_MACHINES), "%s (%d из %d)",
              again ? "участник подключился повторно" : "процесс подключился", L->alive, L->expected);
        source = NULL;

        if (w->stage == WORK)
        {
            start_work(w, w->remaining);
        }
    }
}

static void list_one(Conn *c, int slot)
{
    Client *w = &clients[slot];
    char msg[128];
    snprintf(msg, sizeof msg, "INFO %s %d: %s\n", roles[slot / MAX_MACHINES], slot % MAX_MACHINES,
             w->banned ? "запрещён" : w->conn ? "подключён"
                                              : "не подключён");
    queue(c, msg);
}

static void command(Conn *c, const char *op, int slot)
{
    if (!c->local)
    {
        finish(c, 1, "Управление доступно с 127.0.0.1 на компьютере сервера");
        return;
    }
    if (strcmp(op, "LIST") == 0)
    {
        for (int idx = 0; idx < n_workers; idx++)
        {
            list_one(c, workers[idx]);
        }
        for (int idx = WATCH_SLOT; idx < WATCH_SLOT + MAX_WATCH; idx++)
        {
            if (clients[idx].seen || clients[idx].banned)
            {
                list_one(c, idx);
            }
        }
    }
    else
    {
        Client *w = &clients[slot];
        w->banned = strcmp(op, "DROP") == 0;

        if (w->banned && w->conn)
        {
            Conn *gone = w->conn;
            detach(gone);
            finish(gone, 0, "Клиент отключён оператором");
        }

        list_one(c, slot);
    }
    finish(c, 0, "Команда выполнена");
}

static void request(Conn *c, const char *msg)
{
    if (c->slot >= 0)
    {
        if (c->slot < WATCH_SLOT && strcmp(msg, "DONE") == 0)
        {
            complete(c);
        }
        else
        {
            detach(c);
            finish(c, 1, "Неизвестный запрос клиента");
        }
        return;
    }

    char op[16], role[24], who[24], extra[2];
    int n = sscanf(msg, "%15s %23s %23s %1s", op, role, who, extra);
    int slot = n == 3 ? client_slot(role, who) : -1;

    if (n == 2 && strcmp(op, "WATCH") == 0 && (slot = client_slot("observer", role)) >= 0)
    {
        join(c, slot);
    }
    else if (slot >= 0 && slot < WATCH_SLOT && strcmp(op, "WORKER") == 0)
    {
        join(c, slot);
    }
    else if (n == 1 && strcmp(op, "LIST") == 0)
    {
        command(c, op, -1);
    }
    else if (slot >= 0 && (strcmp(op, "DROP") == 0 || strcmp(op, "ALLOW") == 0))
    {
        command(c, op, slot);
    }
    else
    {
        finish(c, 1, "Неизвестная команда, роль или номер клиента");
    }
}

static void accept_one(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int client = accept(fd, (struct sockaddr *)&addr, &len);

    if (client < 0)
        return;
    int yes = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);

    if (fcntl(client, F_SETFL, O_NONBLOCK) == 0)
    {
        for (int idx = 0; idx < MAX_CONN; idx++)
        {
            Conn *c = &connections[idx];
            if (c->fd < 0)
            {
                c->fd = client;
                c->slot = -1;
                c->local = (ntohl(addr.sin_addr.s_addr) >> 24) == 127;
                c->opened = clock_ms();
                c->closing = c->in.size = c->from = c->size = c->eof_sent = c->overflow = 0;
                return;
            }
        }
    }
    close(client);
}

static void service(Conn *c, short flags)
{
    if (flags & (POLLIN | POLLHUP | POLLERR))
    {
        int r = net_read(c->fd, &c->in);
        if (r < 0)
        {
            close_conn(c);
            return;
        }
        if (c->closing)
        {
            c->in.size = 0;
        }
        else if (r > 0)
        {
            char msg[NET_LINE];
            while (!c->closing && (r = net_line(&c->in, msg)) > 0)
            {
                if (!L->stop)
                {
                    request(c, msg);
                }
                if (L->stop)
                {
                    break;
                }
            }
            if (r < 0)
            {
                detach(c);
                finish(c, 1, "Некорректная строка протокола");
            }
        }
    }
    if (flags & POLLOUT)
    {
        ssize_t n = send(c->fd, c->out + c->from, (size_t)c->size, MSG_NOSIGNAL);
        if (n > 0)
        {
            c->size -= (int)n;
            c->from = c->size ? c->from + (int)n : 0;
        }
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        {
            close_conn(c);
            return;
        }
    }
    long now = clock_ms();

    if (c->overflow || (c->closing && now >= c->closing) ||
        (c->slot < 0 && !c->closing && now - c->opened >= wait_ms))
    {
        close_conn(c);
    }
    else if (c->closing && !c->size && !c->eof_sent)
    {
        shutdown(c->fd, SHUT_WR);
        c->eof_sent = 1;
    }
}

static int network(int fd)
{
    struct pollfd fds[1 + MAX_CONN];
    fds[0] = (struct pollfd){fd, POLLIN, 0};

    for (int idx = 0; idx < MAX_CONN; idx++)
    {
        Conn *c = &connections[idx];
        fds[idx + 1] = (struct pollfd){c->fd, (short)(POLLIN | (c->size ? POLLOUT : 0)), 0};
    }
    if (poll(fds, 1 + MAX_CONN, 20) < 0)
    {
        return errno == EINTR ? 1 : -1;
    }

    int alive = 0;
    for (int idx = 0; idx < MAX_CONN; idx++)
    {
        if (connections[idx].fd >= 0)
        {
            service(&connections[idx], fds[idx + 1].revents);
            alive += connections[idx].fd >= 0;
        }
    }
    if (!L->stop && (fds[0].revents & POLLIN))
    {
        accept_one(fd);
    }

    return alive;
}

static void progress(long opened)
{
    if (interrupted)
    {
        L->signal_no = interrupted;
        event("СИСТЕМА", "Линия", "получен сигнал %s - остановка", interrupted == SIGINT ? "SIGINT" : "SIGTERM");
        stop_all(END_SIGNAL, interrupted == SIGINT ? "получен сигнал SIGINT (Ctrl+C)" : "получен сигнал SIGTERM");
        return;
    }
    long now = clock_ms();

    if (!L->started)
    {
        if (L->alive == L->expected)
        {
            line_start();
            start_work(&clients[0], (long)L->cfg.release_interval * L->tick_ms);
        }
        else if (now - opened >= wait_ms)
        {
            stop_all(END_IMPOSSIBLE, "не все участники подключились до начала работы");
        }

        return;
    }
    if (now_t() >= L->cfg.max_time)
    {
        stop_all(END_TIMEOUT, "достигнут лимит модельного времени");
        return;
    }

    for (int idx = 0; idx < n_workers; idx++)
    {
        Client *w = &clients[workers[idx]];
        if (w->conn && w->stage == WORK && now - w->due >= wait_ms)
        {
            Conn *late = w->conn;
            detach(late);
            finish(late, 1, "Сервер не дождался ответа участника");
        }
        if (!w->conn && now - w->missing >= wait_ms)
        {
            stop_all(END_IMPOSSIBLE, "участник не переподключился вовремя");
            return;
        }
    }

    int changed;
    do
    {
        changed = 0;
        for (int idx = 0; idx < n_workers; idx++)
        {
            changed |= assign(workers[idx]);
        }
    } while (changed);

    if (L->alive != L->expected)
    {
        return;
    }
    for (int idx = 0; idx < n_workers; idx++)
    {
        if (clients[workers[idx]].stage == WORK)
        {
            return;
        }
    }
    diagnose();
    stop_all(L->end, L->why);
}

static int listen_at(const struct sockaddr_in *addr)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;

    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, (const struct sockaddr *)addr, sizeof *addr) < 0 || listen(fd, MAX_CONN) < 0)
    {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }

    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        usage(argv[0]);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }

    struct sockaddr_in addr;
    if (net_addr(&addr, argv[1], argv[2]) < 0)
    {
        return 1;
    }
    const char *cfg_path = NULL, *log_path = "line.log";
    long seed = -1, tick = 50, limit = 0;
    int list = 0, opt;

    while ((opt = getopt(argc - 2, argv + 2, "c:s:d:l:qt:w:Lh")) != -1)
    {
        long value;
        switch (opt)
        {
        case 'c':
            cfg_path = optarg;
            continue;
        case 'l':
            log_path = strcmp(optarg, "-") ? optarg : NULL;
            continue;
        case 'L':
            list = 1;
            continue;
        case 'q':
            quiet = 1;
            continue;
        case 'h':
            usage(argv[0]);
            return 0;
        case 's':
        case 'd':
        case 't':
        case 'w':
            value = number(optarg, opt == 's' ? 0 : 1,
                           opt == 'd' ? 1000 : opt == 'w' ? 300
                                                          : INT_MAX);
            if (value < 0)
            {
                dprintf(2, "Некорректное значение параметра -%c: '%s'\n", opt, optarg);
                return 1;
            }

            if (opt == 's')
                seed = value;
            if (opt == 'd')
                tick = value;
            if (opt == 't')
                limit = value;
            if (opt == 'w')
                wait_ms = value * 1000;

            break;
        default:
            return 1;
        }
    }

    if (optind != argc - 2)
    {
        usage(argv[0]);
        return 1;
    }

    Config cfg;
    char err[300] = "не удалось прочитать конфигурацию";

    if (config_load(&cfg, cfg_path, err, sizeof err) < 0)
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
    if (limit)
    {
        cfg.max_time = limit;
    }

    int code = config_check(&cfg, err, sizeof err);
    long raw = 0;

    for (int idx = 0; idx < cfg.n_pt; idx++)
    {
        raw += cfg.pt[idx].raw;
    }
    if (!code && raw > MAX_PARTS)
    {
        snprintf(err, sizeof err, "заготовок всего больше %d", MAX_PARTS);
        code = 1;
    }
    if (code)
    {
        config_print(&cfg, config_line);
        dprintf(2, "%s: %s\n", code == 1 ? "Ошибка в данных" : "Производство невозможно", err);
        return code;
    }

    line_init(&line, &cfg, (int)tick);
    for (int idx = 0; idx < R_COUNT; idx++)
    {
        for (int idx_actor = 0; idx_actor < line_expected(idx); idx_actor++)
        {
            workers[n_workers++] = idx * MAX_MACHINES + idx_actor;
            if (list)
            {
                dprintf(1, "%s %d\n", roles[idx], idx_actor);
            }
        }
    }

    if (list)
    {
        return 0;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int fd = listen_at(&addr);

    if (fd < 0)
    {
        perror("TCP-сервер");
        return 1;
    }
    if (log_path && (log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
    {
        perror(log_path);
        close(fd);
        return 1;
    }

    for (int idx = 0; idx < MAX_CONN; idx++)
    {
        connections[idx].fd = -1;
    }

    log_init(L->tick_ms, &L->start_clock, &L->events, output);
    config_print(&cfg, config_line);
    dprintf(1, "Сервер слушает %s:%s. Ожидаются участники (%d).\n", argv[1], argv[2], L->expected);
    long opened = clock_ms();

    while (!L->stop)
    {
        if (network(fd) < 0)
        {
            stop_all(END_IMPOSSIBLE, "ошибка poll");
        }
        if (!L->stop)
        {
            progress(opened);
        }
    }

    close(fd);
    check_final();
    code = exit_code();

    for (int idx = 0; idx < n_workers; idx++)
    {
        int slot = workers[idx];
        Conn *c = clients[slot].conn;
        if (c && !c->closing)
        {
            source = c;
            event("СИСТЕМА", actor_name(slot / MAX_MACHINES, slot % MAX_MACHINES), "процесс завершился");
            source = NULL;
            finish(c, code, L->why);
        }
    }

    report();
    for (int idx = 0; idx < MAX_CONN; idx++)
    {
        Conn *c = &connections[idx];
        if (c->fd >= 0 && !c->closing)
        {
            finish(c, code, c->slot >= WATCH_SLOT ? NULL : L->why);
        }
    }
    while (network(-1) > 0)
    {
    }
    for (int idx = 0; idx < MAX_CONN; idx++)
    {
        if (connections[idx].fd >= 0)
        {
            close_conn(&connections[idx]);
        }
    }
    if (log_fd >= 0)
    {
        close(log_fd);
    }
    
    return code;
}
