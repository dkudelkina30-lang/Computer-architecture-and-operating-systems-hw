//-------------------------------------------------------------------------
//  состояние сборочной линии и операции участников
//-------------------------------------------------------------------------
//  Путь детали:
//    склад -(робот)-> очередь станков типа A -> станок A -> выход станка
//    -(робот)-> ... -> вход ОТК -> контролёр -> выход ОТК
//    -(робот)-> комплект (годна) или очередь станка операции k (переделка);
//    списанная деталь выбывает. Полный комплект -> сборочный пост -> изделие.
//
//  Все функции этого файла вызываются только под замком линии
//  (или в процессе-менеджере, который выполняет операции по одной).
//-------------------------------------------------------------------------

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "model.h"
#include "sync.h"

Line *L = NULL; // сборочная линия

static const char *pname(int pid, char *buf)
{
    if (pid < 0)
        return "-";
    snprintf(buf, 48, "%s#%d", L->cfg.pt[L->parts[pid].type].name, pid + 1);
    return buf;
}

static void violation(const char *text)
{
    L->violations++;
    event("!ИНВАРИАНТ", "Проверка", "%s", text);
}

const char *role_name(int role)
{
    static const char *names[] = {"склад", "станок", "контролёр", "робот", "пост"};
    return role >= 0 && role < R_COUNT ? names[role] : "?";
}

// Накопитель по номеру: 0 склад, 1 вход ОТК, 2 выход ОТК, дальше очереди, комплекты и выходы станков
static Store *store_at(int k)
{
    if (k == 0)
        return &L->warehouse;
    if (k == 1)
        return &L->qc_in;
    if (k == 2)
        return &L->qc_out;
    k -= 3;
    if (k < MAX_TYPES)
        return &L->queue[k];
    k -= MAX_TYPES;
    if (k < MAX_TYPES)
        return &L->kit[k];
    return &L->mach[k - MAX_TYPES].out;
}

static int store_no(const Store *s)
{
    for (int k = 0; k < MAX_STORES; k++)
    {
        if (store_at(k) == s)
        {
            return k;
        }
    }
    return -1;
}

static void wake_all(void)
{
    for (int k = 0; k <= L->expected; k++)
    {
        for (int id = S_START; id < L->n_sems; id++)
        {
            sync_post(id);
        }
    }
}

void stop_all(int reason, const char *why)
{
    if (L->end == END_NONE)
    {
        L->end = reason;
        snprintf(L->why, sizeof L->why, "%s", why);
    }
    if (!L->stop)
    {
        L->stop = 1;
        wake_all();
    }
}

static int next_sem;

static void store_init(Store *s, const char *name, Loc kind, int cap)
{
    snprintf(s->name, sizeof s->name, "%s", name);
    s->kind = kind;
    s->cap = cap;
    s->sem = next_sem;
    L->sem_init[next_sem] = cap < 0 ? 0 : cap; // свободные места
    L->sem_init[next_sem + 1] = 0;             // готовые детали
    next_sem += 2;
}

// Положить деталь (место уже получено через семафор свободных мест)
static void put(Store *s, int pid)
{
    s->items[s->count++] = pid;
    if (s->count > s->max)
        s->max = s->count;
    L->parts[pid].loc = s->kind;
}

// Взять деталь с позиции i
static int take(Store *s, int i)
{
    int pid = s->items[i];
    for (int k = i; k < s->count - 1; k++)
    {
        s->items[k] = s->items[k + 1];
    }
    s->count--;
    return pid;
}

// Куда деталь должна ехать дальше
static Store *dest(const Part *p)
{
    const PartType *t = &L->cfg.pt[p->type];
    if (p->passed)
        return &L->kit[p->type]; // годна - в комплект
    if (p->step < t->route_len)
        return &L->queue[t->route[p->step].mtype];
    return &L->qc_in; // маршрут пройден - на ОТК
}

// Создание линии
void line_init(Line *l, const Config *c, int tick_ms, int quiet, const char *log_path)
{
    L = l;
    memset(L, 0, sizeof *L);
    L->cfg = *c;
    L->tick_ms = tick_ms;
    L->quiet = quiet;
    snprintf(L->log_path, sizeof L->log_path, "%s", log_path ? log_path : "");
    L->sem_init[S_LOCK] = 1;
    next_sem = S_FIRST_STORE;

    char nm[48];
    store_init(&L->warehouse, "склад заготовок", L_WAREHOUSE, -1);
    store_init(&L->qc_in, "вход ОТК", L_QC_IN, c->qc_in_cap);
    store_init(&L->qc_out, "выход ОТК", L_QC_OUT, c->qc_out_cap);
    snprintf(L->wh.name, sizeof L->wh.name, "Склад");

    for (int m = 0; m < c->n_mt; m++)
    {
        snprintf(nm, sizeof nm, "очередь %s", c->mt[m].name);
        store_init(&L->queue[m], nm, L_QUEUE, c->queue_cap);
        for (int k = 0; k < c->mt[m].count; k++)
        {
            Machine *mc = &L->mach[L->n_mach++];
            mc->type = m;
            mc->a.part = -1;
            snprintf(mc->a.name, sizeof mc->a.name, "%s-%d", c->mt[m].name, k + 1);
            snprintf(nm, sizeof nm, "выход %s", mc->a.name);
            store_init(&mc->out, nm, L_OUTBUF, c->outbuf_cap);
        }
    }
    for (int t = 0; t < c->n_pt; t++)
    {
        snprintf(nm, sizeof nm, "комплект %s", c->pt[t].name);
        store_init(&L->kit[t], nm, L_KIT, c->kit_cap);
        L->raw_left[t] = c->pt[t].raw;
        for (int p = 0; p < c->n_pr; p++)
        {
            L->required[t] += c->pr[p].plan * c->pr[p].need[t];
        }
    }
    for (int i = 0; i < c->inspectors; i++)
    {
        L->insp[i].a.part = -1;
        L->insp[i].seed = (unsigned)c->seed + 7919u * (unsigned)(i + 1);
        snprintf(L->insp[i].a.name, sizeof L->insp[i].a.name, "ОТК-%d", i + 1);
    }
    for (int i = 0; i < c->robots; i++)
    {
        L->rob[i].part = -1;
        snprintf(L->rob[i].name, sizeof L->rob[i].name, "РОБОТ-%d", i + 1);
    }
    for (int i = 0; i < c->stations; i++)
    {
        snprintf(L->st[i].a.name, sizeof L->st[i].a.name, "ПОСТ-%d", i + 1);
    }

    L->n_sems = next_sem;
    L->expected = 1 + L->n_mach + c->inspectors + c->robots + c->stations;
}

int line_expected(int role)
{
    int n[] = {1, L->n_mach, L->cfg.inspectors, L->cfg.robots, L->cfg.stations};
    return role >= 0 && role < R_COUNT ? n[role] : 0;
}

static Actor *actor(int role, int i)
{
    switch (role)
    {
    case R_WAREHOUSE:
        return &L->wh;
    case R_MACHINE:
        return &L->mach[i].a;
    case R_INSPECTOR:
        return &L->insp[i].a;
    case R_ROBOT:
        return &L->rob[i];
    default:
        return &L->st[i].a;
    }
}

// Все процессы подключились - запускаем часы и открываем семафор «старт»
void line_start(void)
{
    L->started = 1;
    L->start_clock = clock_ms();
    L->last_event_ms = 0;
    event("СИСТЕМА", "Линия", "запуск: станков %d, контролёров %d, роботов %d, постов %d - %d процессов",
          L->n_mach, L->cfg.inspectors, L->cfg.robots, L->cfg.stations, L->expected);
    for (int i = 0; i < L->expected; i++)
        sync_post(S_START);
}

// Подключение
static int hello(int role, int i, int out[4])
{
    if (role < 0 || role >= R_COUNT || i < 0 || i >= line_expected(role))
        return -1; // нет такого
    Actor *a = actor(role, i);
    if (a->joined)
        return -2; // такой процесс уже подключён
    if (L->stop || L->started)
        return -3; // линия уже работает или остановлена

    a->joined = 1;
    L->alive++;
    event("СИСТЕМА", a->name, "процесс подключился (%d из %d)", L->alive, L->expected);
    const Config *c = &L->cfg;
    out[0] = out[1] = out[2] = 0;
    out[3] = L->tick_ms;

    switch (role)
    { // на каких семафорах ждать и сколько работать
    case R_WAREHOUSE:
        out[0] = S_RELEASE;
        out[1] = c->release_interval;
        break;
    case R_MACHINE:
        out[0] = L->queue[L->mach[i].type].sem + 1;
        out[1] = L->mach[i].out.sem;
        break;
    case R_INSPECTOR:
        out[0] = L->qc_in.sem + 1;
        out[1] = L->qc_out.sem;
        out[2] = c->inspect_time;
        break;
    case R_ROBOT:
        out[0] = S_ROBOT;
        out[2] = c->transport_time;
        break;
    case R_STATION:
        out[0] = S_KIT;
        out[2] = c->assembly_time;
        break;
    }
    return 0;
}

static int bye(int role, int i)
{
    if (role < 0 || role >= R_COUNT || i < 0 || i >= line_expected(role))
        return -1;
    Actor *a = actor(role, i);

    if (!a->joined)
        return -1;
    a->joined = 0;
    L->alive--;
    event("СИСТЕМА", a->name, "процесс завершился");
    return 0;
}

// Склад
static int w_release(void)
{
    const Config *c = &L->cfg;
    char pn[48];
    if (L->stop)
        return -1;
    int t = -1; // какой тип выпускать (по очереди)

    for (int k = 0; k < c->n_pt && !L->soft_stop; k++)
    {
        int tt = (L->release_rr + k) % c->n_pt;
        if (L->raw_left[tt] > 0 && L->consumed[tt] + L->on_line[tt] < L->required[tt])
        {
            t = tt;
            break;
        }
    }
    if (t < 0)
        return 0; // выпускать нечего - склад будет ждать

    L->release_rr = t + 1;
    int pid = L->n_parts++;
    L->parts[pid] = (Part){t, 0, 0, 0, now_t(), L_WAREHOUSE};
    L->raw_left[t]--;
    L->on_line[t]++;
    L->created[t]++;
    L->wh.done++;
    put(&L->warehouse, pid);
    event("СОЗДАНИЕ", "Склад", "создана деталь %s, осталось заготовок %d", pname(pid, pn), L->raw_left[t]);
    event("ОЧЕРЕДЬ", "Склад", "%s ждёт отправки на %s", pn, c->mt[c->pt[t].route[0].mtype].name);
    sync_post(S_ROBOT); // роботам появилась работа
    return 1;
}

// Станок
//  Взять деталь из очереди. Возвращает длительность операции, 0 - деталей нет
static int m_take(Machine *m)
{
    Store *q = &L->queue[m->type];
    char pn[48];

    if (L->stop)
        return -1;
    if (q->count == 0)
        return 0;

    int pid = take(q, 0);
    Part *p = &L->parts[pid];
    const PartType *t = &L->cfg.pt[p->type];
    L->checks += 2;

    if (p->step >= t->route_len || t->route[p->step].mtype != m->type)
        violation("деталь попала на станок не по порядку маршрута");
    if (m->a.part != -1)
        violation("станок уже обрабатывает деталь");

    m->a.part = pid;
    p->loc = L_MACHINE;
    int dur = t->route[p->step].duration;
    event("ОП.НАЧАЛО", m->a.name, "операция %d/%d над %s, длительность %d",
          p->step + 1, t->route_len, pname(pid, pn), dur);
    sync_post(q->sem); // в очереди освободилось место
    sync_post(S_ROBOT);
    m->a.from_ms = now_ms();

    return dur;
}

static void m_put(Machine *m)
{
    char pn[48];
    put(&m->out, m->a.part);
    event("ОЧЕРЕДЬ", m->a.name, "%s -> выходной накопитель (%d/%d)", pname(m->a.part, pn), m->out.count, m->out.cap);
    m->a.part = -1;
    sync_post(S_ROBOT); // деталь можно увозить
}

// Операция закончена. 1 - деталь отдана, 0 - выход занят, станок ждёт места
static int m_done(Machine *m)
{
    Part *p = &L->parts[m->a.part];
    char pn[48];
    m->a.busy_ms += now_ms() - m->a.from_ms;
    p->step++; // маршрут продвигается только здесь
    m->a.done++;
    event("ОП.КОНЕЦ", m->a.name, "операция %d/%d над %s завершена", p->step,
          L->cfg.pt[p->type].route_len, pname(m->a.part, pn));

    if (L->stop)
        return -1;
    if (sync_trywait(m->out.sem) == 0)
    { // место в выходном накопителе есть
        m_put(m);
        return 1;
    }

    event("ПРОСТОЙ", m->a.name, "выходной накопитель заполнен - станок приостановлен, %s на станке", pn);
    m->a.from_ms = now_ms();
    return 0;
}

// Место освободилось (станок дождался семафора)
static int m_resume(Machine *m)
{
    m->a.blocked_ms += now_ms() - m->a.from_ms;

    if (L->stop)
        return -1; // деталь остаётся на станке

    event("ПРОСТОЙ", m->a.name, "место освободилось - станок возобновляет работу");
    m_put(m);
    return 0;
}

// Контролёр ОТК
static int i_take(Inspector *in)
{
    char pn[48];

    if (L->stop)
        return -1;
    if (L->qc_in.count == 0)
        return 0;

    int pid = take(&L->qc_in, 0);
    in->a.part = pid;
    in->a.done++;
    L->parts[pid].loc = L_INSPECTOR;
    event("КОНТРОЛЬ", in->a.name, "начало контроля %s", pname(pid, pn));
    sync_post(L->qc_in.sem);
    sync_post(S_ROBOT);
    in->a.from_ms = now_ms();
    return 1;
}

static void i_put(Inspector *in)
{
    put(&L->qc_out, in->a.part);
    in->a.part = -1;
    sync_post(S_ROBOT);
}

// Решение по детали. 0 - готово, 2 - выход ОТК занят, контролёр ждёт места
static int i_decide(Inspector *in)
{
    Part *p = &L->parts[in->a.part];
    const PartType *t = &L->cfg.pt[p->type];
    char pn[48];
    pname(in->a.part, pn);
    in->a.busy_ms += now_ms() - in->a.from_ms;
    int bad = rand_r(&in->seed) / ((double)RAND_MAX + 1.0) < t->defect; // случайный брак

    if (!bad)
    {
        p->passed = 1;
        event("КОНТРОЛЬ", in->a.name, "%s годна - направляется на сборку", pn);
    }
    else
    {
        in->defects++;
        L->defects[p->type]++;
        event("БРАК", in->a.name, "в %s обнаружен брак", pn);
        if (t->rework_step >= 0 && p->reworks < t->max_rework)
        {
            p->reworks++;
            L->reworks[p->type]++;
            p->step = t->rework_step; // назад на нужную операцию
            event("ПЕРЕДЕЛКА", in->a.name, "%s возвращается на операцию %d (%s), переделка %d из %d", pn,
                  t->rework_step + 1, L->cfg.mt[t->route[t->rework_step].mtype].name, p->reworks, t->max_rework);
        }
        else
        {
            p->loc = L_SCRAP; // списание
            L->on_line[p->type]--;
            L->scrapped[p->type]++;
            in->a.part = -1;
            event("СПИСАНИЕ", in->a.name, "%s списана: %s", pn,
                  t->rework_step < 0 || t->max_rework == 0 ? "переделка не предусмотрена" : "исчерпан лимит переделок");
            sync_post(S_RELEASE); // складу - выпустить замену
            return 0;
        }
    }

    if (L->stop)
        return -1;
    if (sync_trywait(L->qc_out.sem) == 0)
    {
        i_put(in);
        return 0;
    }

    event("ПРОСТОЙ", in->a.name, "выход ОТК заполнен - контролёр ждёт, %s у него", pn);
    in->a.from_ms = now_ms();
    return 2;
}

static int i_wait_done(Inspector *in)
{
    in->a.blocked_ms += now_ms() - in->a.from_ms;
    if (L->stop)
        return -1; // деталь остаётся у контролёра
    i_put(in);
    return 0;
}

// Транспортный робот
//  Найти в накопителе s деталь, для которой удалось забронировать место
static int try_store(Store *s, int *idx, Store **to)
{
    for (int i = 0; i < s->count; i++)
    {
        Store *d = dest(&L->parts[s->items[i]]);
        if (sync_trywait(d->sem) == 0)
        { // место забронировано
            *idx = i;
            *to = d;
            return 1;
        }
    }
    return 0;
}

// Откуда везти: сначала выход ОТК, потом выходы станков, потом склад
static Store *find_job(int *idx, Store **to)
{
    if (try_store(&L->qc_out, idx, to))
    {
        return &L->qc_out;
    }
    for (int i = 0; i < L->n_mach; i++)
    {
        if (try_store(&L->mach[i].out, idx, to))
        {
            return &L->mach[i].out;
        }
    }
    if (try_store(&L->warehouse, idx, to))
    {
        return &L->warehouse;
    }
    return NULL;
}

static int r_take(Actor *r)
{
    char pn[48];
    if (L->stop)
    {
        return -1;
    }
    int idx;
    Store *to = NULL;
    Store *from = find_job(&idx, &to);
    if (!from)
    {
        return 0;
    } // работы нет
    int pid = take(from, idx);
    r->part = pid;
    r->dest = store_no(to); // куда везём
    r->done++;
    L->parts[pid].loc = L_ROBOT;
    event("ПЕРЕВОЗКА", r->name, "взял %s: %s -> %s", pname(pid, pn), from->name, to->name);
    if (from->cap >= 0)
    {
        sync_post(from->sem);
    } // у источника освободилось место
    sync_post(S_ROBOT); // вдруг работа есть и для другого робота
    r->from_ms = now_ms();
    return 1;
}

static int r_deliver(Actor *r)
{
    Store *to = store_at(r->dest);
    int pid = r->part;
    char pn[48];
    r->busy_ms += now_ms() - r->from_ms;
    put(to, pid); // место было забронировано заранее
    r->part = -1;
    event("ПЕРЕВОЗКА", r->name, "доставил %s -> %s", pname(pid, pn), to->name);

    if (to->kind == L_KIT)
    {
        L->lead_sum[L->parts[pid].type] += now_t() - L->parts[pid].born;
        L->lead_n[L->parts[pid].type]++;
        event("КОМПЛЕКТ", "Сборка", "%s поступила в комплект (%d/%d)", pn, to->count, to->cap);
        sync_post(S_KIT); // посты проверят комплект
    }
    else
    {
        event("ОЧЕРЕДЬ", r->name, "%s поставлена: %s (%d/%d)", pn, to->name, to->count, to->cap);
        sync_post(to->sem + 1); // станок или ОТК может брать
    }

    return 0;
}

// Сборочный пост
static int s_take(Station *s)
{
    const Config *c = &L->cfg;
    char pn[48];
    if (L->stop)
    {
        return -1;
    }
    int pr = -1; // какое изделие можно собрать

    for (int i = 0; i < c->n_pr && pr < 0; i++)
    {
        if (L->produced[i] + L->in_asm[i] >= c->pr[i].plan)
        {
            continue;
        }
        int complete = 1;
        for (int t = 0; t < c->n_pt; t++)
        {
            if (L->kit[t].count < c->pr[i].need[t])
            {
                complete = 0;
            }
        }
        if (complete)
        {
            pr = i;
        }
    }

    if (pr < 0)
    {
        return 0;
    } // полного комплекта нет

    char list[512] = "";
    int len = 0;
    s->n = 0;
    s->kit_born = now_t();

    for (int t = 0; t < c->n_pt; t++)
    { // забираем весь комплект разом
        for (int k = 0; k < c->pr[pr].need[t]; k++)
        {
            int pid = take(&L->kit[t], 0);
            if (L->parts[pid].born < s->kit_born)
            {
                s->kit_born = L->parts[pid].born;
            }
            L->checks++;
            if (!L->parts[pid].passed)
            {
                violation("в комплект попала непроверенная деталь");
            }
            L->parts[pid].loc = L_ASSEMBLY;
            s->parts[s->n++] = pid;
            sync_post(L->kit[t].sem); // в комплекте освободилось место
            if (len < (int)sizeof list - 64)
            {
                len += snprintf(list + len, sizeof list - (size_t)len, "%s%s", len ? ", " : "", pname(pid, pn));
            }
        }
    }

    L->in_asm[pr]++;
    s->product = pr;
    s->number = ++L->product_no;
    event("СБОРКА", s->a.name, "начало сборки %s №%d из комплекта: %s", c->pr[pr].name, s->number, list);
    sync_post(S_ROBOT);
    sync_post(S_KIT); // другой пост тоже может проверить
    s->a.from_ms = now_ms();
    return 1;
}

static int s_done(Station *s)
{
    const Config *c = &L->cfg;
    int pr = s->product;
    s->a.busy_ms += now_ms() - s->a.from_ms;

    for (int k = 0; k < s->n; k++)
    {
        Part *p = &L->parts[s->parts[k]];
        p->loc = L_PRODUCT;
        L->consumed[p->type]++;
        L->on_line[p->type]--;
    }

    s->n = 0;
    s->a.done++;
    L->produced[pr]++;
    L->in_asm[pr]--;
    L->cycle_sum += now_t() - s->kit_born;
    event("ВЫПУСК", s->a.name, "выпущено изделие %s №%d, по плану %d/%d",
          c->pr[pr].name, s->number, L->produced[pr], c->pr[pr].plan);
    int done = 1;

    for (int i = 0; i < c->n_pr; i++)
    {
        if (L->produced[i] < c->pr[i].plan)
        {
            done = 0;
        }
    }
    if (done)
    {
        stop_all(END_PLAN, "все изделия по плану выпущены");
    }

    return 0;
}

// Выполнение операции
//  Номер участника для операций - его индекс, для HELLO/BYE - роль*1000 + индекс
int line_op(int op, int who, int out[4])
{
    switch (op)
    {
    case OP_HELLO:
        return hello(who / 1000, who % 1000, out);
    case OP_BYE:
        return bye(who / 1000, who % 1000);
    case OP_W_RELEASE:
        return w_release();
    case OP_M_TAKE:
        return m_take(&L->mach[who]);
    case OP_M_DONE:
        return m_done(&L->mach[who]);
    case OP_M_RESUME:
        return m_resume(&L->mach[who]);
    case OP_I_TAKE:
        return i_take(&L->insp[who]);
    case OP_I_DECIDE:
        return i_decide(&L->insp[who]);
    case OP_I_PUT:
        return i_wait_done(&L->insp[who]);
    case OP_R_TAKE:
        return r_take(&L->rob[who]);
    case OP_R_DELIVER:
        return r_deliver(&L->rob[who]);
    case OP_S_TAKE:
        return s_take(&L->st[who]);
    case OP_S_DONE:
        return s_done(&L->st[who]);
    }
    return -1;
}

void diagnose(void)
{
    const Config *c = &L->cfg;
    int stuck = 0, n = 0;
    for (int i = 0; i < L->n_parts; i++)
    {
        Loc l = L->parts[i].loc;
        if (l != L_KIT && l != L_PRODUCT && l != L_SCRAP)
        {
            stuck++;
        }
    }
    for (int t = 0; t < c->n_pt; t++)
    { // сначала - нехватка заготовок
        if (L->raw_left[t] == 0 && L->consumed[t] + L->on_line[t] < L->required[t])
        {
            L->end = END_RAW;
            n += snprintf(L->why + n, sizeof L->why - (size_t)n, "%sзаготовки %s исчерпаны (списано %d)",
                          n ? "; " : "", c->pt[t].name, L->scrapped[t]);
        }
    }
    if (L->end == END_RAW)
    {
        return;
    }
    if (L->alive < L->expected)
    { // кто-то из участников ушёл раньше времени
        L->end = END_IMPOSSIBLE;
        snprintf(L->why, sizeof L->why, "процессов-участников осталось %d из %d", L->alive, L->expected);
        return;
    }
    if (stuck > 0)
    { // детали остались, но не двигаются
        L->end = END_DEADLOCK;
        n = snprintf(L->why, sizeof L->why, "на линии застряло деталей: %d;", stuck);

        for (int i = 0; i < L->n_mach && n < (int)sizeof L->why - 48; i++)
        {
            if (L->mach[i].a.part >= 0)
            {
                n += snprintf(L->why + n, sizeof L->why - (size_t)n, " %s приостановлен;", L->mach[i].a.name);
            }
        }
        for (int i = 0; i < c->inspectors && n < (int)sizeof L->why - 48; i++)
        {
            if (L->insp[i].a.part >= 0)
            {
                n += snprintf(L->why + n, sizeof L->why - (size_t)n, " %s ждёт места;", L->insp[i].a.name);
            }
        }

        return;
    }
    L->end = END_IMPOSSIBLE;
    snprintf(L->why, sizeof L->why, "из оставшихся деталей не собрать полный комплект");
}

// Лимит времени и «тишина»: если событий давно нет - линия встала
void watchdog(void)
{
    const Config *c = &L->cfg;
    if (!L->started || L->stop)
    {
        return;
    }
    int longest = c->inspect_time + c->transport_time + c->assembly_time + c->release_interval;

    for (int t = 0; t < c->n_pt; t++)
    {
        for (int s = 0; s < c->pt[t].route_len; s++)
        {
            if (c->pt[t].route[s].duration > longest)
            {
                longest = c->pt[t].route[s].duration;
            }
        }
    }

    long quiet_ms = (2L * longest + 5) * L->tick_ms;

    if (now_t() > c->max_time)
    {
        stop_all(END_TIMEOUT, "достигнут лимит модельного времени");
    }
    else if (now_ms() - L->last_event_ms > quiet_ms)
    {
        if (L->soft_stop)
        {
            L->end = END_SOFT;
            snprintf(L->why, sizeof L->why, "детали, которые были в работе, доделаны");
        }
        else
        {
            diagnose();
        }
        stop_all(L->end, L->why);
    }
}

// После завершения всех процессов: каждая деталь должна быть ровно в одном месте
void check_final(void)
{
    static int seen[MAX_PARTS];
    memset(seen, 0, sizeof seen);
    int ns = 3 + L->cfg.n_mt + L->cfg.n_pt + L->n_mach;

    for (int k = 0; k < MAX_STORES; k++)
    {
        Store *s = store_at(k);
        if (s->name[0] == '\0')
            continue; // накопитель не используется
        if (s->cap >= 0 && s->count > s->cap)
            violation("накопитель переполнен");
        for (int i = 0; i < s->count; i++)
        {
            int pid = s->items[i];
            seen[pid]++;
            if (L->parts[pid].loc != s->kind)
                violation("у детали записано не то место");
            if (s->kind == L_KIT && !L->parts[pid].passed)
                violation("в комплекте непроверенная деталь");
        }
    }
    for (int i = 0; i < L->n_mach; i++)
    {
        if (L->mach[i].a.part >= 0)
        {
            seen[L->mach[i].a.part]++;
        }
    }
    for (int i = 0; i < L->cfg.inspectors; i++)
    {
        if (L->insp[i].a.part >= 0)
        {
            seen[L->insp[i].a.part]++;
        }
    }
    for (int i = 0; i < L->cfg.robots; i++)
    {
        if (L->rob[i].part >= 0)
        {
            seen[L->rob[i].part]++;
        }
    }
    for (int i = 0; i < L->cfg.stations; i++)
    {
        for (int k = 0; k < L->st[i].n; k++)
        {
            seen[L->st[i].parts[k]]++;
        }
    }

    L->checks += ns + L->n_parts;

    for (int i = 0; i < L->n_parts; i++)
    {
        int gone = L->parts[i].loc == L_PRODUCT || L->parts[i].loc == L_SCRAP;
        if (seen[i] != (gone ? 0 : 1))
        {
            violation("деталь найдена не в одном месте");
        }
    }
}

// Текущее состояние линии
void print_status(void)
{
    const Config *c = &L->cfg;
    char buf[2048], pn[48];
    int n = snprintf(buf, sizeof buf, "\n--- Состояние линии, t=%ld, процессов %d ---\nВыпуск:", now_t(), L->alive);
    for (int i = 0; i < c->n_pr; i++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s %d/%d", c->pr[i].name, L->produced[i], c->pr[i].plan);
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\nСклад: %d, вход ОТК: %d/%d, выход ОТК: %d/%d\nСтанки:",
                  L->warehouse.count, L->qc_in.count, L->qc_in.cap, L->qc_out.count, L->qc_out.cap);
    for (int i = 0; i < L->n_mach && n < (int)sizeof buf - 100; i++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s[%s]", L->mach[i].a.name, pname(L->mach[i].a.part, pn));
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\nРоботы:");
    for (int i = 0; i < c->robots && n < (int)sizeof buf - 100; i++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s[%s]", L->rob[i].name, pname(L->rob[i].part, pn));
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\nКомплекты:");
    for (int t = 0; t < c->n_pt && n < (int)sizeof buf - 100; t++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s %d/%d", c->pt[t].name, L->kit[t].count, L->kit[t].cap);
    }
    snprintf(buf + n, sizeof buf - (size_t)n, "\n---\n");
    say("%s", buf);
}

static double pct(long part, long total) { return total > 0 ? 100.0 * part / total : 0.0; }

// Итоговая статистика
void report(void)
{
    const Config *c = &L->cfg;
    long T = now_ms(), tick = L->tick_ms;
    static const char *reason[] = {"не определено", "ПЛАН ВЫПОЛНЕН", "ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ",
                                   "ПРОИЗВОДСТВО НЕВОЗМОЖНО (взаимная блокировка)", "ПРОИЗВОДСТВО НЕВОЗМОЖНО",
                                   "ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ", "ПРЕРВАНО СИГНАЛОМ", "МЯГКАЯ ОСТАНОВКА (SIGUSR2)"};
    say("\n========== ИТОГИ МОДЕЛИРОВАНИЯ ==========\n");
    say("Завершение: %s\nПричина: %s\n", reason[L->end], L->why);
    say("Время работы: %ld ед. (%.1f с), процессов участников: %d\n", T / tick, T / 1000.0, L->expected);
    say("Событий: %ld, seed: %ld\n", L->events, c->seed);
    say("Проверок правил: %ld, нарушений правил модели: %d\n", L->checks, L->violations);
    int total = 0;

    for (int p = 0; p < c->n_pr; p++)
    {
        say("Изделие %s: выпущено %d из %d\n", c->pr[p].name, L->produced[p], c->pr[p].plan);
        total += L->produced[p];
    }
    if (total)
    {
        say("Средний цикл изделия: %.1f, такт выпуска: %.1f\n", (double)L->cycle_sum / total,
            (double)(T / tick) / total);
    }
    for (int t = 0; t < c->n_pt; t++)
    {
        say("Деталь %s: создано %d, брак %d, переделок %d, списано %d, в изделиях %d, на линии %d, "
            "ср. путь до комплекта %.1f\n",
            c->pt[t].name, L->created[t], L->defects[t], L->reworks[t],
            L->scrapped[t], L->consumed[t], L->on_line[t],
            L->lead_n[t] ? (double)L->lead_sum[t] / L->lead_n[t] : 0.0);
    }
    for (int i = 0; i < L->n_mach; i++)
    {
        say("Станок %s: операций %d, работа %.1f%%, приостановлен %.1f%%\n", L->mach[i].a.name,
            L->mach[i].a.done, pct(L->mach[i].a.busy_ms, T), pct(L->mach[i].a.blocked_ms, T));
    }
    for (int i = 0; i < c->inspectors; i++)
    {
        say("Контролёр %s: проверено %d, брак %d, работа %.1f%%, ожидание места %.1f%%\n", L->insp[i].a.name,
            L->insp[i].a.done, L->insp[i].defects, pct(L->insp[i].a.busy_ms, T), pct(L->insp[i].a.blocked_ms, T));
    }
    for (int i = 0; i < c->robots; i++)
    {
        say("Робот %s: рейсов %d, работа %.1f%%\n", L->rob[i].name, L->rob[i].done, pct(L->rob[i].busy_ms, T));
    }
    for (int i = 0; i < c->stations; i++)
    {
        say("Пост %s: собрано %d, работа %.1f%%\n", L->st[i].a.name, L->st[i].a.done, pct(L->st[i].a.busy_ms, T));
    }
    say("Максимальное заполнение: склад %d, вход ОТК %d/%d, выход ОТК %d/%d\n",
        L->warehouse.max, L->qc_in.max, L->qc_in.cap, L->qc_out.max, L->qc_out.cap);
    say("=========================================\n");
}

// Код завершения программы по причине остановки
int exit_code(void)
{
    static const int codes[] = {3, 0, 2, 3, 3, 4, 0, 0};
    int code = codes[L->end];

    if (L->end == END_SIGNAL)
        code = 128 + L->signal_no;
    if (L->end == END_SOFT)
        code = 128 + SIGUSR2;
    if (code == 0 && L->violations)
        code = 5;

    return code;
}
