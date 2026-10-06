//  Чтение и проверка входных параметров
//----------------------------------------------------------------------
//  Формат файла ('#' — комментарий до конца строки):
//    machine <ИМЯ> <количество>
//    part <ИМЯ> raw <N> defect <P> rework <k|none> max_rework <N>
//               route <СТАНОК:время> <СТАНОК:время> ...
//    product <ИМЯ> plan <N> kit <ДЕТАЛЬ:кол-во> ...
//    <параметр> <значение>              (robots 2, kit_capacity 4, ...)
//  Номер операции в rework считается с 1. Строки machine должны идти
//  раньше part, а part — раньше product.
//-----------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

// Встроенная конфигурация: выпуск редукторов(вал + 2 шестерни + корпус)
static const char *BUILTIN =
    "machine LATHE 1\n"
    "machine MILL  2\n"
    "machine DRILL 1\n"
    "machine GRIND 1\n"
    "part SHAFT   raw 6  defect 0.10 rework 2    max_rework 1 route LATHE:4 GRIND:3\n"
    "part GEAR    raw 12 defect 0.15 rework 2    max_rework 2 route MILL:5 DRILL:2 GRIND:2\n"
    "part HOUSING raw 5  defect 0.08 rework none max_rework 0 route MILL:6 DRILL:3\n"
    "product REDUCER plan 4 kit SHAFT:1 GEAR:2 HOUSING:1\n"
    "robots 2\n"
    "transport_time 2\n"
    "outbuf_capacity 1\n"
    "qc_queue_capacity 3\n"
    "release_interval 2\n";

// Найти скалярный параметр по имени: адрес поля и минимальное значение
static int *scalar(Config *c, const char *key, int *min)
{
    struct
    {
        const char *key;
        int *field;
        int min;
    } t[] = {
        {"inspectors", &c->inspectors, 0},
        {"inspect_time", &c->inspect_time, 1},
        {"stations", &c->stations, 0},
        {"assembly_time", &c->assembly_time, 1},
        {"robots", &c->robots, 0},
        {"transport_time", &c->transport_time, 1},
        {"queue_capacity", &c->queue_cap, 0},
        {"outbuf_capacity", &c->outbuf_cap, 0},
        {"qc_queue_capacity", &c->qc_in_cap, 0},
        {"qc_out_capacity", &c->qc_out_cap, 0},
        {"kit_capacity", &c->kit_cap, 0},
        {"release_interval", &c->release_interval, 0},
    };

    for (size_t idx = 0; idx < sizeof t / sizeof t[0]; idx++)
        if (strcmp(key, t[idx].key) == 0)
        {
            *min = t[idx].min;
            return t[idx].field;
        }
    return NULL;
}

// Строка -> неотрицательное целое, ошибка, если в строке не только число
static int to_int(const char *s, int *v)
{
    char *end;
    errno = 0;
    long x = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno || x < 0 || x > 1000000000)
    {
        return -1;
    }

    *v = (int)x;
    return 0;
}

// "ИМЯ:число" -> имя и число (двоеточие заменяется концом строки)
static int pair(char *tok, char **name, int *v)
{
    char *colon = strchr(tok, ':');
    if (!colon)
    {
        return -1;
    }

    *colon = '\0';
    *name = tok;
    return to_int(colon + 1, v);
}

static int find_mt(const Config *c, const char *name)
{
    for (int idx = 0; idx < c->n_mt; idx++)
    {
        if (strcmp(c->mt[idx].name, name) == 0)
        {
            return idx;
        }
    }
    return -1;
}

static int find_pt(const Config *c, const char *name)
{
    for (int idx = 0; idx < c->n_pt; idx++)
    {
        if (strcmp(c->pt[idx].name, name) == 0)
        {
            return idx;
        }
    }
    return -1;
}

// Записать сообщение "файл:строка: текст" и вернуть -1
static int fail(char *err, int len, const char *src, int line, const char *fmt, ...)
{
    int n = snprintf(err, (size_t)len, "%s:%d: ", src, line);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + n, (size_t)(len - n), fmt, ap);
    va_end(ap);
    return -1;
}
#define FAIL(...) return fail(err, errlen, src, line, __VA_ARGS__)

// Разбор текста конфигурации (текст изменяется: в него вписываются '\0')
static int parse(Config *c, char *text, const char *src, char *err, int errlen)
{
    int line = 0;
    for (char *s = text, *next; s && *s; s = next)
    {
        next = strchr(s, '\n');
        if (next)
        {
            *next++ = '\0';
        }

        line++;
        char *hash = strchr(s, '#');
        if (hash)
        {
            *hash = '\0';
        }

        char *tk[64], *save;
        int n = 0;
        for (char *t = strtok_r(s, " \t\r", &save); t; t = strtok_r(NULL, " \t\r", &save))
        {
            if (n == 64)
            {
                FAIL("слишком длинная строка");
            }
            tk[n++] = t;
        }

        if (n == 0)
        {
            continue;
        }

        if (strcmp(tk[0], "machine") == 0)
        {
            if (n != 3)
            {
                FAIL("ожидается: machine <имя> <количество>");
            }

            if (c->n_mt == MAX_TYPES || strlen(tk[1]) >= MAX_NAME || find_mt(c, tk[1]) >= 0)
            {
                FAIL("недопустимый или повторный тип станка '%s'", tk[1]);
            }

            MachineType *m = &c->mt[c->n_mt];
            strcpy(m->name, tk[1]);
            if (to_int(tk[2], &m->count))
            {
                FAIL("некорректное количество станков '%s'", tk[2]);
            }
            c->n_mt++;
        }
        else if (strcmp(tk[0], "part") == 0)
        {
            if (n < 2 || c->n_pt == MAX_TYPES || strlen(tk[1]) >= MAX_NAME || find_pt(c, tk[1]) >= 0)
            {
                FAIL("недопустимое или повторное имя детали");
            }

            PartType *p = &c->pt[c->n_pt];
            memset(p, 0, sizeof *p);
            strcpy(p->name, tk[1]);
            int rework = 0;
            for (int idx = 2; idx < n; idx += 2)
            {
                if (strcmp(tk[idx], "route") == 0)
                {
                    for (idx++; idx < n; idx++)
                    {
                        char *mname;
                        int d, k;

                        if (pair(tk[idx], &mname, &d) || d == 0)
                        {
                            FAIL("операция маршрута: СТАНОК:время>0");
                        }
                        if ((k = find_mt(c, mname)) < 0)
                        {
                            FAIL("неизвестный тип станка '%s' в маршруте", mname);
                        }
                        if (p->route_len == MAX_ROUTE)
                        {
                            FAIL("слишком длинный маршрут");
                        }

                        p->route[p->route_len++] = (Step){k, d};
                    }
                    break;
                }

                if (idx + 1 >= n)
                {
                    FAIL("у параметра '%s' нет значения", tk[idx]);
                }
                const char *key = tk[idx], *val = tk[idx + 1];
                char *end;
                if (strcmp(key, "raw") == 0)
                {
                    if (to_int(val, &p->raw))
                    {
                        FAIL("некорректный запас заготовок '%s'", val);
                    }
                }
                else if (strcmp(key, "defect") == 0)
                {
                    p->defect = strtod(val, &end);
                    if (*end || p->defect < 0 || p->defect > 1)
                    {
                        FAIL("вероятность брака должна быть в [0,1]");
                    }
                }
                else if (strcmp(key, "rework") == 0)
                {
                    if (strcmp(val, "none") != 0 && (to_int(val, &rework) || rework == 0))
                    {
                        FAIL("rework: номер операции (с 1) или none");
                    }
                }
                else if (strcmp(key, "max_rework") == 0)
                {
                    if (to_int(val, &p->max_rework))
                    {
                        FAIL("некорректное max_rework '%s'", val);
                    }
                }
                else
                {
                    FAIL("неизвестный параметр детали '%s'", key);
                }
            }
            if (p->route_len == 0)
            {
                FAIL("не задан маршрут детали '%s'", p->name);
            }
            if (rework > p->route_len)
            {
                FAIL("rework больше длины маршрута");
            }
            p->rework_step = rework - 1;
            c->n_pt++;
        }
        else if (strcmp(tk[0], "product") == 0)
        {
            if (n < 2 || c->n_pr == MAX_PRODUCTS || strlen(tk[1]) >= MAX_NAME)
            {
                FAIL("недопустимое имя изделия");
            }

            Product *pr = &c->pr[c->n_pr];
            memset(pr, 0, sizeof *pr);
            strcpy(pr->name, tk[1]);
            int idx = 2, kit = 0;

            if (idx + 1 < n && strcmp(tk[idx], "plan") == 0)
            {
                if (to_int(tk[idx + 1], &pr->plan))
                {
                    FAIL("некорректный план выпуска");
                }
                idx += 2;
            }

            if (idx >= n || strcmp(tk[idx], "kit") != 0)
            {
                FAIL("ожидается: product <имя> plan <N> kit <ДЕТАЛЬ:кол-во> ...");
            }

            for (idx++; idx < n; idx++)
            {
                char *pname;
                int q, k;

                if (pair(tk[idx], &pname, &q) || q == 0)
                {
                    FAIL("элемент комплекта: ДЕТАЛЬ:кол-во>0");
                }
                if ((k = find_pt(c, pname)) < 0)
                {
                    FAIL("неизвестная деталь '%s' в комплекте", pname);
                }
                pr->need[k] += q;
                kit += q;
            }
            if (kit == 0 || kit > MAX_KIT)
            {
                FAIL("в комплекте должно быть от 1 до %d деталей", MAX_KIT);
            }
            c->n_pr++;
        }
        else
        {
            int v, min = 0, *field;
            if (n != 2 || to_int(tk[1], &v))
            {
                FAIL("ожидается: %s <целое>", tk[0]);
            }
            if (strcmp(tk[0], "seed") == 0)
            {
                c->seed = v;
            }
            else if (strcmp(tk[0], "max_time") == 0 && v > 0)
            {
                c->max_time = v;
            }
            else if ((field = scalar(c, tk[0], &min)) != NULL && v >= min)
            {
                *field = v;
            }
            else
            {
                FAIL("неизвестный параметр или недопустимое значение '%s'", tk[0]);
            }
        }
    }
    return 0;
}

// Прочитать файл целиком системными вызовами open/read/close
static char *read_file(const char *path, char *err, int errlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        snprintf(err, (size_t)errlen, "не удалось открыть '%s': %s", path, strerror(errno));
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *text = malloc(cap);
    for (;;)
    {
        if (len + 1 >= cap)
        {
            char *t = realloc(text, cap *= 2);
            if (!t)
            {
                free(text);
            }
            text = t;
        }
        if (!text)
        {
            close(fd);
            snprintf(err, (size_t)errlen, "нет памяти");
            return NULL;
        }

        ssize_t r = read(fd, text + len, cap - len - 1);
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        if (r < 0)
        {
            snprintf(err, (size_t)errlen, "ошибка чтения '%s': %s", path, strerror(errno));
            free(text);
            close(fd);
            return NULL;
        }
        if (r == 0)
        {
            break;
        }
        len += (size_t)r;
    }
    close(fd);
    text[len] = '\0';
    return text;
}

int config_load(Config *c, const char *path, char *err, int errlen)
{
    memset(c, 0, sizeof *c);
    c->inspectors = 1;
    c->inspect_time = 1;
    c->stations = 1;
    c->assembly_time = 5;
    c->robots = 2;
    c->transport_time = 2;
    c->queue_cap = 3;
    c->outbuf_cap = 2;
    c->qc_in_cap = 4;
    c->qc_out_cap = 2;
    c->kit_cap = 4;
    c->release_interval = 1;
    c->seed = -1;
    c->max_time = 1000000;

    char *text = path ? read_file(path, err, errlen) : strdup(BUILTIN);
    if (!text)
    {
        return -1;
    }

    int rc = parse(c, text, path ? path : "<встроенная>", err, errlen);
    free(text);
    return rc;
}

// Если условие истинно, записать сообщение и вернуть код
#define CHECK(cond, code, ...)                      \
    if (cond)                                       \
    {                                               \
        snprintf(err, (size_t)errlen, __VA_ARGS__); \
        return code;                                \
    }

int config_check(const Config *c, char *err, int errlen)
{
    int machines = 0, plan = 0;
    for (int idx = 0; idx < c->n_mt; idx++)
    {
        machines += c->mt[idx].count;
    }

    for (int idx = 0; idx < c->n_pr; idx++)
        plan += c->pr[idx].plan;
    CHECK(!c->n_mt || !c->n_pt || !c->n_pr, 1, "нужны хотя бы один станок, одна деталь и одно изделие");
    CHECK(machines > MAX_MACHINES || c->inspectors > MAX_ACTORS || c->robots > MAX_ACTORS || c->stations > MAX_ACTORS,
          1, "слишком много участников (станков до %d, прочих до %d)", MAX_MACHINES, MAX_ACTORS);
    CHECK(plan == 0, 1, "план выпуска пуст");

    // Далее данные корректны, но производство может быть невозможно
    CHECK(!c->robots, 3, "нет транспортных роботов: детали некому перемещать");
    CHECK(!c->inspectors, 3, "нет контролёров ОТК: ни одна деталь не пройдёт контроль");
    CHECK(!c->stations, 3, "нет сборочных постов");
    CHECK(!c->queue_cap || !c->outbuf_cap || !c->qc_in_cap || !c->qc_out_cap, 3,
          "нулевая вместимость накопителя: детали негде разместить");
    for (int p = 0; p < c->n_pr; p++)
    {
        if (c->pr[p].plan == 0)
        {
            continue;
        }
        for (int t = 0; t < c->n_pt; t++)
        {
            const PartType *pt = &c->pt[t];
            int need = c->pr[p].need[t];
            if (need == 0)
            {
                continue;
            }

            for (int s = 0; s < pt->route_len; s++)
            {
                CHECK(c->mt[pt->route[s].mtype].count == 0, 3, "для операции %d детали %s нет ни одного станка %s",
                      s + 1, pt->name, c->mt[pt->route[s].mtype].name);
            }

            CHECK(need > c->kit_cap, 3, "комплект %s требует %d шт. %s, а накопитель вмещает %d",
                  c->pr[p].name, need, pt->name, c->kit_cap);
            CHECK(pt->raw < need, 3, "заготовок %s не хватает даже на один комплект %s", pt->name, c->pr[p].name);
        }
    }
    return 0;
}

void config_print(const Config *c, int fd)
{
    dprintf(fd, "Параметры: роботов %d (перевозка %d), контролёров %d (контроль %d), постов %d (сборка %d)\n",
            c->robots, c->transport_time, c->inspectors, c->inspect_time, c->stations, c->assembly_time);
    dprintf(fd, "Накопители: очередь станка %d, выход станка %d, вход ОТК %d, выход ОТК %d, комплект %d\n",
            c->queue_cap, c->outbuf_cap, c->qc_in_cap, c->qc_out_cap, c->kit_cap);
    for (int t = 0; t < c->n_pt; t++)
    {
        const PartType *p = &c->pt[t];
        dprintf(fd, "Деталь %s: заготовок %d, брак %.0f%%, ", p->name, p->raw, p->defect * 100);
        if (p->rework_step >= 0 && p->max_rework > 0)
        {
            dprintf(fd, "переделка с операции %d до %d раз, маршрут:", p->rework_step + 1, p->max_rework);
        }
        else
        {
            dprintf(fd, "брак списывается, маршрут:");
        }
        for (int s = 0; s < p->route_len; s++)
        {
            dprintf(fd, " %s(%d)", c->mt[p->route[s].mtype].name, p->route[s].duration);
        }
        dprintf(fd, "\n");
    }

    for (int idx = 0; idx < c->n_pr; idx++)
    {
        dprintf(fd, "Изделие %s: план %d, комплект:", c->pr[idx].name, c->pr[idx].plan);
        for (int t = 0; t < c->n_pt; t++)
        {
            if (c->pr[idx].need[t])
            {
                dprintf(fd, " %s x%d", c->pt[t].name, c->pr[idx].need[t]);
            }
        }
        dprintf(fd, "\n");
    }
}
