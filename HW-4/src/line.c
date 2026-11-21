//-------------------------------------------------------------------------
//  состояние сборочной линии и операции участников
//-------------------------------------------------------------------------
//  Путь детали:
//    склад -(робот)-> очередь станков типа A -> станок A -> выход станка
//    -(робот)-> ... -> вход ОТК -> контролёр -> выход ОТК
//    -(робот)-> комплект (годна) или очередь станка операции k (переделка);
//    списанная деталь выбывает. Полный комплект -> сборочный пост -> изделие.
//
//  Сервер выполняет операции по одной, поэтому здесь нет замков.
//-------------------------------------------------------------------------

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "model.h"

Line *L = NULL; // сборочная линия

static const char *pname(int pid, char *buf)
{
    if (pid < 0)
    {
        return "-";
    }
    snprintf(buf, 48, "%s#%d", L->cfg.pt[L->parts[pid].type].name, pid + 1);
    return buf;
}

static void violation(const char *text)
{
    L->violations++;
    event("!ИНВАРИАНТ", "Проверка", "%s", text);
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
    for (int idx_item = 0; idx_item < MAX_STORES; idx_item++)
    {
        if (store_at(idx_item) == s)
        {
            return idx_item;
        }
    }
    return -1;
}

void stop_all(int reason, const char *why)
{
    if (L->end == END_NONE)
    {
        L->end = reason;
        snprintf(L->why, sizeof L->why, "%s", why);
    }
    L->stop = 1;
}

static void store_init(Store *s, const char *name, Loc kind, int cap)
{
    snprintf(s->name, sizeof s->name, "%s", name);
    s->kind = kind;
    s->cap = cap;
}

int store_room(const Store *s)
{
    return s->cap < 0 || s->count + s->reserved < s->cap;
}

// Положить деталь (место уже проверено или забронировано)
static void put(Store *s, int pid)
{
    s->items[s->count++] = pid;
    if (s->count > s->max)
    {
        s->max = s->count;
    }
    L->parts[pid].loc = s->kind;
}

// Взять деталь с позиции i
static int take(Store *s, int i)
{
    int pid = s->items[i];
    for (int idx_item = i; idx_item < s->count - 1; idx_item++)
    {
        s->items[idx_item] = s->items[idx_item + 1];
    }
    s->count--;
    return pid;
}

// Куда деталь должна ехать дальше
static Store *dest(const Part *p)
{
    const PartType *t = &L->cfg.pt[p->type];
    if (p->passed)
    {
        return &L->kit[p->type];
    }
    if (p->step < t->route_len)
    {
        return &L->queue[t->route[p->step].mtype];
    }
    return &L->qc_in;
}

// Создание линии
void line_init(Line *l, const Config *c, int tick_ms)
{
    L = l;
    memset(L, 0, sizeof *L);
    L->cfg = *c;
    L->tick_ms = tick_ms;

    char nm[48];
    store_init(&L->warehouse, "склад заготовок", L_WAREHOUSE, -1);
    store_init(&L->qc_in, "вход ОТК", L_QC_IN, c->qc_in_cap);
    store_init(&L->qc_out, "выход ОТК", L_QC_OUT, c->qc_out_cap);
    snprintf(L->wh.name, sizeof L->wh.name, "Склад");

    for (int idx_machine = 0; idx_machine < c->n_mt; idx_machine++)
    {
        snprintf(nm, sizeof nm, "очередь %s", c->mt[idx_machine].name);
        store_init(&L->queue[idx_machine], nm, L_QUEUE, c->queue_cap);
        for (int idx_item = 0; idx_item < c->mt[idx_machine].count; idx_item++)
        {
            Machine *mc = &L->mach[L->n_mach++];
            mc->type = idx_machine;
            mc->a.part = -1;
            snprintf(mc->a.name, sizeof mc->a.name, "%s-%d", c->mt[idx_machine].name, idx_item + 1);
            snprintf(nm, sizeof nm, "выход %s", mc->a.name);
            store_init(&mc->out, nm, L_OUTBUF, c->outbuf_cap);
        }
    }
    for (int idx_type = 0; idx_type < c->n_pt; idx_type++)
    {
        snprintf(nm, sizeof nm, "комплект %s", c->pt[idx_type].name);
        store_init(&L->kit[idx_type], nm, L_KIT, c->kit_cap);
        L->raw_left[idx_type] = c->pt[idx_type].raw;
        for (int idx_prod = 0; idx_prod < c->n_pr; idx_prod++)
        {
            L->required[idx_type] += (long)c->pr[idx_prod].plan * c->pr[idx_prod].need[idx_type];
        }
    }
    for (int idx = 0; idx < c->inspectors; idx++)
    {
        L->insp[idx].a.part = -1;
        L->insp[idx].seed = (unsigned)c->seed + 7919u * (unsigned)(idx + 1);
        snprintf(L->insp[idx].a.name, sizeof L->insp[idx].a.name, "ОТК-%d", idx + 1);
    }
    for (int idx = 0; idx < c->robots; idx++)
    {
        L->rob[idx].part = -1;
        snprintf(L->rob[idx].name, sizeof L->rob[idx].name, "РОБОТ-%d", idx + 1);
    }
    for (int idx = 0; idx < c->stations; idx++)
    {
        snprintf(L->st[idx].a.name, sizeof L->st[idx].a.name, "ПОСТ-%d", idx + 1);
    }

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

const char *actor_name(int role, int who)
{
    return actor(role, who)->name;
}

// Все процессы подключились - запускаем часы
void line_start(void)
{
    L->started = 1;
    L->start_clock = clock_ms();
    event("СИСТЕМА", "Линия", "запуск: станков %d, контролёров %d, роботов %d, постов %d - %d процессов",
          L->n_mach, L->cfg.inspectors, L->cfg.robots, L->cfg.stations, L->expected);
}

// Склад
static int w_release(void)
{
    const Config *c = &L->cfg;
    char pn[48];

    if (L->stop)
    {
        return -1;
    }

    int t = -1; // какой тип выпускать (по очереди)

    for (int idx = 0; idx < c->n_pt; idx++)
    {
        int tt = (L->release_rr + idx) % c->n_pt;

        if (L->raw_left[tt] > 0 && L->consumed[tt] + L->on_line[tt] < L->required[tt])
        {
            t = tt;
            break;
        }
    }
    if (t < 0)
    {
        return 0;
    }

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
    return 1;
}

// Станок
//  Взять деталь из очереди. Возвращает длительность операции, 0 - деталей нет
static int m_take(Machine *m)
{
    Store *q = &L->queue[m->type];
    char pn[48];

    if (L->stop)
    {
        return -1;
    }
    if (q->count == 0)
    {
        return 0;
    }

    int pid = take(q, 0);
    Part *p = &L->parts[pid];
    const PartType *t = &L->cfg.pt[p->type];
    L->checks += 2;

    if (p->step >= t->route_len || t->route[p->step].mtype != m->type)
    {
        violation("деталь попала на станок не по порядку маршрута");
    }
    if (m->a.part != -1)
    {
        violation("станок уже обрабатывает деталь");
    }

    m->a.part = pid;
    p->loc = L_MACHINE;
    int dur = t->route[p->step].duration;
    event("ОП.НАЧАЛО", m->a.name, "операция %d/%d над %s, длительность %d",
          p->step + 1, t->route_len, pname(pid, pn), dur);
    m->a.from_ms = now_ms();

    return dur;
}

static void m_put(Machine *m)
{
    char pn[48];
    put(&m->out, m->a.part);
    event("ОЧЕРЕДЬ", m->a.name, "%s -> выходной накопитель (%d/%d)", pname(m->a.part, pn), m->out.count, m->out.cap);
    m->a.part = -1;
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
    if (store_room(&m->out))
    { // место в выходном накопителе есть
        m_put(m);
        return 1;
    }

    event("ПРОСТОЙ", m->a.name, "выходной накопитель заполнен - станок приостановлен, %s на станке", pn);
    m->a.from_ms = now_ms();
    return 0;
}

// Место на выходе освободилось
static int m_resume(Machine *m)
{
    m->a.blocked_ms += now_ms() - m->a.from_ms;

    if (L->stop)
    {
        return -1;
    }

    event("ПРОСТОЙ", m->a.name, "место освободилось - станок возобновляет работу");
    m_put(m);
    return 0;
}

// Контролёр ОТК
static int i_take(Inspector *in)
{
    char pn[48];

    if (L->stop)
    {
        return -1;
    }
    if (L->qc_in.count == 0)
    {
        return 0;
    }

    int pid = take(&L->qc_in, 0);
    in->a.part = pid;
    in->a.done++;
    L->parts[pid].loc = L_INSPECTOR;
    event("КОНТРОЛЬ", in->a.name, "начало контроля %s", pname(pid, pn));
    in->a.from_ms = now_ms();
    return 1;
}

static void i_put(Inspector *in)
{
    put(&L->qc_out, in->a.part);
    in->a.part = -1;
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
            return 0;
        }
    }

    if (L->stop)
    {
        return -1;
    }
    if (store_room(&L->qc_out))
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
    {
        return -1;
    }
    i_put(in);
    return 0;
}

// Транспортный робот
//  Найти в накопителе s деталь, для которой удалось забронировать место
static int try_store(Store *s, int *item, Store **to)
{
    for (int idx = 0; idx < s->count; idx++)
    {
        Store *d = dest(&L->parts[s->items[idx]]);
        if (store_room(d))
        { // место забронировано
            d->reserved++;
            *item = idx;
            *to = d;
            return 1;
        }
    }
    return 0;
}

// Откуда везти: сначала выход ОТК, потом выходы станков, потом склад
static Store *find_job(int *item, Store **to)
{
    if (try_store(&L->qc_out, item, to))
    {
        return &L->qc_out;
    }
    for (int idx = 0; idx < L->n_mach; idx++)
    {
        if (try_store(&L->mach[idx].out, item, to))
        {
            return &L->mach[idx].out;
        }
    }
    if (try_store(&L->warehouse, item, to))
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
    r->dest = store_no(to);
    r->done++;
    L->parts[pid].loc = L_ROBOT;
    event("ПЕРЕВОЗКА", r->name, "взял %s: %s -> %s", pname(pid, pn), from->name, to->name);
    r->from_ms = now_ms();
    return 1;
}

static int r_deliver(Actor *r)
{
    Store *to = store_at(r->dest);
    int pid = r->part;
    char pn[48];
    r->busy_ms += now_ms() - r->from_ms;
    to->reserved--;
    put(to, pid); // место было забронировано заранее
    r->part = -1;
    event("ПЕРЕВОЗКА", r->name, "доставил %s -> %s", pname(pid, pn), to->name);

    if (to->kind == L_KIT)
    {
        L->lead_sum[L->parts[pid].type] += now_t() - L->parts[pid].born;
        L->lead_n[L->parts[pid].type]++;
        event("КОМПЛЕКТ", "Сборка", "%s поступила в комплект (%d/%d)", pn, to->count, to->cap);
    }
    else
    {
        event("ОЧЕРЕДЬ", r->name, "%s поставлена: %s (%d/%d)", pn, to->name, to->count, to->cap);
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

    for (int idx = 0; idx < c->n_pr && pr < 0; idx++)
    {
        if (L->produced[idx] + L->in_asm[idx] >= c->pr[idx].plan)
        {
            continue;
        }
        int complete = 1;
        for (int idx_type = 0; idx_type < c->n_pt; idx_type++)
        {
            if (L->kit[idx_type].count < c->pr[idx].need[idx_type])
            {
                complete = 0;
            }
        }
        if (complete)
        {
            pr = idx;
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

    for (int idx_type = 0; idx_type < c->n_pt; idx_type++)
    { // забираем весь комплект разом
        for (int idx_item = 0; idx_item < c->pr[pr].need[idx_type]; idx_item++)
        {
            int pid = take(&L->kit[idx_type], 0);
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
    s->a.from_ms = now_ms();
    return 1;
}

static int s_done(Station *s)
{
    const Config *c = &L->cfg;
    int pr = s->product;
    s->a.busy_ms += now_ms() - s->a.from_ms;

    for (int idx_item = 0; idx_item < s->n; idx_item++)
    {
        Part *p = &L->parts[s->parts[idx_item]];
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

    for (int idx = 0; idx < c->n_pr; idx++)
    {
        if (L->produced[idx] < c->pr[idx].plan)
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
// Номер участника - его индекс внутри роли
int line_op(int op, int who)
{
    switch (op)
    {
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
    for (int idx = 0; idx < L->n_parts; idx++)
    {
        Loc l = L->parts[idx].loc;
        if (l != L_KIT && l != L_PRODUCT && l != L_SCRAP)
        {
            stuck++;
        }
    }
    for (int idx_type = 0; idx_type < c->n_pt; idx_type++)
    { // сначала - нехватка заготовок
        if (n < (int)sizeof L->why - 110 && L->raw_left[idx_type] == 0 &&
            L->consumed[idx_type] + L->on_line[idx_type] < L->required[idx_type])
        {
            L->end = END_RAW;
            n += snprintf(L->why + n, sizeof L->why - (size_t)n, "%sзаготовки %s исчерпаны (списано %d)",
                          n ? "; " : "", c->pt[idx_type].name, L->scrapped[idx_type]);
        }
    }
    if (L->end == END_RAW)
    {
        return;
    }
    if (stuck > 0)
    { // детали остались, но не двигаются
        L->end = END_DEADLOCK;
        n = snprintf(L->why, sizeof L->why, "на линии застряло деталей: %d;", stuck);

        for (int idx = 0; idx < L->n_mach && n < (int)sizeof L->why - 48; idx++)
        {
            if (L->mach[idx].a.part >= 0)
            {
                n += snprintf(L->why + n, sizeof L->why - (size_t)n, " %s приостановлен;", L->mach[idx].a.name);
            }
        }
        for (int idx = 0; idx < c->inspectors && n < (int)sizeof L->why - 48; idx++)
        {
            if (L->insp[idx].a.part >= 0)
            {
                n += snprintf(L->why + n, sizeof L->why - (size_t)n, " %s ждёт места;", L->insp[idx].a.name);
            }
        }

        return;
    }
    L->end = END_IMPOSSIBLE;
    snprintf(L->why, sizeof L->why, "из оставшихся деталей не собрать полный комплект");
}

// После завершения всех процессов: каждая деталь должна быть ровно в одном месте
void check_final(void)
{
    static int seen[MAX_PARTS];
    memset(seen, 0, sizeof seen);
    int ns = 3 + L->cfg.n_mt + L->cfg.n_pt + L->n_mach;
    int reserved[MAX_STORES] = {0};
    for (int idx = 0; idx < L->cfg.robots; idx++)
    {
        if (L->rob[idx].part >= 0)
        {
            reserved[L->rob[idx].dest]++;
        }
    }

    for (int idx_item = 0; idx_item < MAX_STORES; idx_item++)
    {
        Store *s = store_at(idx_item);
        if (s->name[0] == '\0')
            continue; // накопитель не используется
        if (s->cap >= 0 && s->count + s->reserved > s->cap)
            violation("накопитель переполнен");
        if (s->reserved != reserved[idx_item])
            violation("число занятых роботами мест не совпадает с бронью накопителя");

        for (int idx = 0; idx < s->count; idx++)
        {
            int pid = s->items[idx];
            seen[pid]++;
            if (L->parts[pid].loc != s->kind)
            {
                violation("у детали записано не то место");
            }
            if (s->kind == L_KIT && !L->parts[pid].passed)
            {
                violation("в комплекте непроверенная деталь");
            }
        }
    }
    for (int idx = 0; idx < L->n_mach; idx++)
    {
        if (L->mach[idx].a.part >= 0)
        {
            seen[L->mach[idx].a.part]++;
        }
    }
    for (int idx = 0; idx < L->cfg.inspectors; idx++)
    {
        if (L->insp[idx].a.part >= 0)
        {
            seen[L->insp[idx].a.part]++;
        }
    }
    for (int idx = 0; idx < L->cfg.robots; idx++)
    {
        if (L->rob[idx].part >= 0)
        {
            seen[L->rob[idx].part]++;
        }
    }
    for (int idx = 0; idx < L->cfg.stations; idx++)
    {
        for (int idx_item = 0; idx_item < L->st[idx].n; idx_item++)
        {
            seen[L->st[idx].parts[idx_item]]++;
        }
    }

    L->checks += ns + L->n_parts;

    for (int idx = 0; idx < L->n_parts; idx++)
    {
        int gone = L->parts[idx].loc == L_PRODUCT || L->parts[idx].loc == L_SCRAP;
        if (seen[idx] != (gone ? 0 : 1))
        {
            violation("деталь найдена не в одном месте");
        }
    }
}

// Текущее состояние линии
void print_status(void)
{
    const Config *c = &L->cfg;
    char pn[48];
    say("--- Состояние линии, t=%ld, участников %d из %d ---\n", now_t(), L->alive, L->expected);

    for (int idx = 0; idx < c->n_pr; idx++)
    {
        say("Выпуск %s: %d/%d\n", c->pr[idx].name, L->produced[idx], c->pr[idx].plan);
    }

    say("Склад: %d, вход ОТК: %d/%d, выход ОТК: %d/%d\n",
        L->warehouse.count, L->qc_in.count, L->qc_in.cap, L->qc_out.count, L->qc_out.cap);

    for (int idx = 0; idx < c->n_mt; idx++)
    {
        say("%s: %d/%d\n", L->queue[idx].name, L->queue[idx].count, L->queue[idx].cap);
    }
    for (int idx = 0; idx < L->n_mach; idx++)
    {
        const Machine *m = &L->mach[idx];
        say("Станок %s[%s], выход %d/%d\n", m->a.name, pname(m->a.part, pn), m->out.count, m->out.cap);
    }
    for (int idx = 0; idx < c->inspectors; idx++)
    {
        say("Контролёр %s[%s]\n", L->insp[idx].a.name, pname(L->insp[idx].a.part, pn));
    }
    for (int idx = 0; idx < c->robots; idx++)
    {
        say("Робот %s[%s]\n", L->rob[idx].name, pname(L->rob[idx].part, pn));
    }
    for (int idx = 0; idx < c->stations; idx++)
    {
        say("Пост %s: деталей на сборке %d\n", L->st[idx].a.name, L->st[idx].n);
    }
    for (int idx = 0; idx < c->n_pt; idx++)
    {
        say("%s: %d/%d\n", L->kit[idx].name, L->kit[idx].count, L->kit[idx].cap);
    }

    say("---\n");
}

static double pct(long part, long total) { return total > 0 ? 100.0 * part / total : 0.0; }

// Итоговая статистика
void report(void)
{
    const Config *c = &L->cfg;
    long T = now_ms(), tick = L->tick_ms;
    static const char *reason[] = {"не определено", "ПЛАН ВЫПОЛНЕН", "ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ",
                                   "ПРОИЗВОДСТВО НЕВОЗМОЖНО (взаимная блокировка)", "ПРОИЗВОДСТВО НЕВОЗМОЖНО",
                                   "ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ", "ПРЕРВАНО СИГНАЛОМ"};
    say("\n========== ИТОГИ МОДЕЛИРОВАНИЯ ==========\n");
    say("Завершение: %s\nПричина: %s\n", reason[L->end], L->why);
    say("Время работы: %ld ед. (%.1f с), процессов участников: %d\n", T / tick, T / 1000.0, L->expected);
    say("Событий: %ld, seed: %ld\n", L->events, c->seed);
    say("Проверок правил: %ld, нарушений правил модели: %d\n", L->checks, L->violations);
    int total = 0;

    for (int idx_prod = 0; idx_prod < c->n_pr; idx_prod++)
    {
        say("Изделие %s: выпущено %d из %d\n", c->pr[idx_prod].name, L->produced[idx_prod], c->pr[idx_prod].plan);
        total += L->produced[idx_prod];
    }
    if (total)
    {
        say("Средний цикл изделия: %.1f, такт выпуска: %.1f\n", (double)L->cycle_sum / total,
            (double)(T / tick) / total);
    }
    for (int idx_type = 0; idx_type < c->n_pt; idx_type++)
    {
        say("Деталь %s: создано %d, брак %d, переделок %d, списано %d, в изделиях %d, на линии %d, "
            "ср. путь до комплекта %.1f\n",
            c->pt[idx_type].name, L->created[idx_type], L->defects[idx_type], L->reworks[idx_type],
            L->scrapped[idx_type], L->consumed[idx_type], L->on_line[idx_type],
            L->lead_n[idx_type] ? (double)L->lead_sum[idx_type] / L->lead_n[idx_type] : 0.0);
    }
    for (int idx = 0; idx < L->n_mach; idx++)
    {
        say("Станок %s: операций %d, работа %.1f%%, приостановлен %.1f%%\n", L->mach[idx].a.name,
            L->mach[idx].a.done, pct(L->mach[idx].a.busy_ms, T), pct(L->mach[idx].a.blocked_ms, T));
    }
    for (int idx = 0; idx < c->inspectors; idx++)
    {
        say("Контролёр %s: проверено %d, брак %d, работа %.1f%%, ожидание места %.1f%%\n", L->insp[idx].a.name,
            L->insp[idx].a.done, L->insp[idx].defects, pct(L->insp[idx].a.busy_ms, T), pct(L->insp[idx].a.blocked_ms, T));
    }
    for (int idx = 0; idx < c->robots; idx++)
    {
        say("Робот %s: рейсов %d, работа %.1f%%\n", L->rob[idx].name, L->rob[idx].done, pct(L->rob[idx].busy_ms, T));
    }
    for (int idx = 0; idx < c->stations; idx++)
    {
        say("Пост %s: собрано %d, работа %.1f%%\n", L->st[idx].a.name, L->st[idx].a.done, pct(L->st[idx].a.busy_ms, T));
    }
    say("Максимальное заполнение: склад %d, вход ОТК %d/%d, выход ОТК %d/%d\n",
        L->warehouse.max, L->qc_in.max, L->qc_in.cap, L->qc_out.max, L->qc_out.cap);
    say("=========================================\n");
}

// Код завершения программы по причине остановки
int exit_code(void)
{
    static const int codes[] = {3, 0, 2, 3, 3, 4, 0};
    int code = codes[L->end];

    if (L->end == END_SIGNAL)
    {
        code = 128 + L->signal_no;
    }
    if (code == 0 && L->violations)
    {
        code = 5;
    }

    return code;
}
