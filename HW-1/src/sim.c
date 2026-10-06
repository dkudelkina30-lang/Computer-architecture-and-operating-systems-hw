//---------------------------------------------------------------------------
//  Поведение участников и продвижение модельного времени
//----------------------------------------------------------------------------
//  Путь детали:
//    склад -(робот)-> очередь станков типа A -> станок A -> выход станка
//    -(робот)-> ... -> вход ОТК -> контролёр -> выход ОТК
//    -(робот)-> комплект (годна)  или  очередь станка операции k (переделка);
//    списанная деталь выбывает. Полный комплект -> сборочный пост -> изделие.
//----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "model.h"

static unsigned long long rng;

static double rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

static void *xrealloc(void *p, size_t size)
{
    p = realloc(p, size);

    if (!p)
    {
        dprintf(2, "Нет памяти\n");
        exit(70);
    }

    return p;
}

const char *label(const Factory *f, int pid)
{
    static char buf[4][48];
    static int k;
    k = (k + 1) % 4;

    if (pid < 0)
    {
        return "-";
    }

    snprintf(buf[k], sizeof buf[k], "%s#%d", f->cfg->pt[f->parts[pid].type].name, pid + 1);
    return buf[k];
}

static void set_state(Factory *f, Actor *a, State st)
{
    if (a->st == BUSY)
    {
        a->busy += f->now - a->since;
    }
    if (a->st == BLOCKED)
    {
        a->blocked += f->now - a->since;
    }
    a->st = st;
    a->since = f->now;
}

static void store_init(Store *s, const char *name, Loc kind, int owner, int cap)
{
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->kind = kind;
    s->owner = owner;
    s->cap = cap;
}

// Сколько ещё можно положить с учётом мест, забронированных роботами
static int space(const Store *s)
{
    return s->cap < 0 ? 1 : s->cap - s->count - s->reserved;
}

// Положить деталь в накопитель и записать в деталь, где она теперь
static void put(Factory *f, Store *s, int pid)
{
    if (s->count == s->alloc)
    {
        s->alloc = s->alloc ? 2 * s->alloc : 8;
        s->items = xrealloc(s->items, (size_t)s->alloc * sizeof(int));
    }

    s->items[s->count++] = pid;
    if (s->count > s->max)
    {
        s->max = s->count;
    }
    f->parts[pid].loc = s->kind;
    f->parts[pid].loc_id = s->owner;
}

// Изъять деталь с позиции idx, сохранив порядок остальных
static int take(Store *s, int idx)
{
    int pid = s->items[idx];
    memmove(&s->items[idx], &s->items[idx + 1], (size_t)(s->count - idx - 1) * sizeof(int));
    s->count--;
    return pid;
}

static void schedule(Factory *f, long dt, EvType type, int who)
{
    f->cal[f->n_cal++] = (Event){f->now + dt, f->seq++, type, who};
}

// Ближайшее событие
// Событий в календаре не больше числа участников, поэтому хватает простого линейного поиска.
int sim_next(Factory *f, Event *e)
{
    if (f->n_cal == 0)
    {
        return 0;
    }

    int best = 0;
    for (int idx = 1; idx < f->n_cal; idx++)
    {
        if (f->cal[idx].time < f->cal[best].time || (f->cal[idx].time == f->cal[best].time && f->cal[idx].seq < f->cal[best].seq))
        {
            best = idx;
        }
    }
    *e = f->cal[best];
    f->cal[best] = f->cal[--f->n_cal];
    return 1;
}

// Куда деталь должна попасть дальше
static Store *dest(Factory *f, const Part *p)
{
    const PartType *t = &f->cfg->pt[p->type];
    if (p->passed)
    {
        return &f->kit[p->type];
    } // годна - в комплект
    if (p->step < t->route_len)
    {
        return &f->queue[t->route[p->step].mtype];
    } // следующая операция
    return &f->qc_in; // маршрут пройден - на ОТК
}

// Инициализация
void sim_init(Factory *f, const Config *c)
{
    memset(f, 0, sizeof *f);
    f->cfg = c;
    rng = (unsigned long long)c->seed * 2654435761ULL + 88172645463325252ULL;

    for (int idx = 0; idx < 8; idx++)
    {
        rnd();
    }

    char nm[64];
    store_init(&f->warehouse, "склад заготовок", L_WAREHOUSE, -1, -1);
    store_init(&f->qc_in, "вход ОТК", L_QC_IN, -1, c->qc_in_cap);
    store_init(&f->qc_out, "выход ОТК", L_QC_OUT, -1, c->qc_out_cap);

    for (int m = 0; m < c->n_mt; m++)
    {
        snprintf(nm, sizeof nm, "очередь %s", c->mt[m].name);
        store_init(&f->queue[m], nm, L_QUEUE, m, c->queue_cap);

        for (int k = 0; k < c->mt[m].count; k++)
        {
            Machine *mc = &f->mach[f->n_mach];
            snprintf(mc->a.name, sizeof mc->a.name, "%s-%d", c->mt[m].name, k + 1);
            snprintf(nm, sizeof nm, "выход %s", mc->a.name);
            store_init(&mc->out, nm, L_OUTBUF, f->n_mach, c->outbuf_cap);
            mc->type = m;
            mc->part = -1;
            f->n_mach++;
        }
    }

    for (int t = 0; t < c->n_pt; t++)
    {
        snprintf(nm, sizeof nm, "комплект %s", c->pt[t].name);
        store_init(&f->kit[t], nm, L_KIT, t, c->kit_cap);
        f->raw_left[t] = c->pt[t].raw;

        for (int p = 0; p < c->n_pr; p++)
        {
            f->required[t] += c->pr[p].plan * c->pr[p].need[t];
        }
    }

    for (int idx = 0; idx < c->inspectors; idx++)
    {
        snprintf(f->insp[idx].a.name, sizeof f->insp[idx].a.name, "ОТК-%d", idx + 1);
        f->insp[idx].part = -1;
    }

    for (int idx = 0; idx < c->robots; idx++)
    {
        snprintf(f->rob[idx].a.name, sizeof f->rob[idx].a.name, "РОБОТ-%d", idx + 1);
        f->rob[idx].part = -1;
    }

    for (int idx = 0; idx < c->stations; idx++)
    {
        snprintf(f->st[idx].a.name, sizeof f->st[idx].a.name, "ПОСТ-%d", idx + 1);
        f->st[idx].product = -1;
    }

    event(0, "СИСТЕМА", "Линия", "запуск: станков %d, контролёров %d, роботов %d, постов %d",
          f->n_mach, c->inspectors, c->robots, c->stations);
    schedule(f, 0, EV_RELEASE, -1);
    f->release_pending = 1;
}

void sim_free(Factory *f)
{
    free(f->warehouse.items);
    free(f->qc_in.items);
    free(f->qc_out.items);

    for (int idx = 0; idx < MAX_TYPES; idx++)
    {
        free(f->queue[idx].items);
        free(f->kit[idx].items);
    }

    for (int idx = 0; idx < f->n_mach; idx++)
        free(f->mach[idx].out.items);

    free(f->parts);
}

// Склад заготовок
// Нужна ли заготовка типа t: есть запас и до плана не хватает
static int needs(const Factory *f, int t)
{
    return f->raw_left[t] > 0 && f->consumed[t] + f->on_line[t] < f->required[t];
}

// Запланировать следующий запуск, если он нужен и ещё не запланирован
static void plan_release(Factory *f)
{
    if (f->release_pending)
        return;

    for (int t = 0; t < f->cfg->n_pt; t++)
    {
        if (needs(f, t))
        {
            schedule(f, f->cfg->release_interval, EV_RELEASE, -1);
            f->release_pending = 1;
            return;
        }
    }
}

static void on_release(Factory *f)
{
    const Config *c = f->cfg;
    f->release_pending = 0;

    for (int k = 0; k < c->n_pt; k++)
    {
        int t = (f->release_rr + k) % c->n_pt;

        if (!needs(f, t))
            continue;

        f->release_rr = t + 1;

        if (f->n_parts == f->cap_parts)
        {
            f->cap_parts = f->cap_parts ? 2 * f->cap_parts : 64;
            f->parts = xrealloc(f->parts, (size_t)f->cap_parts * sizeof(Part));
        }

        int pid = f->n_parts++;
        f->parts[pid] = (Part){.type = t, .loc_id = -1, .created = f->now};
        f->raw_left[t]--;
        f->on_line[t]++;
        f->created[t]++;

        event(f->now, "СОЗДАНИЕ", "Склад", "создана деталь %s, осталось заготовок %d",
              label(f, pid), f->raw_left[t]);
        put(f, &f->warehouse, pid);
        event(f->now, "ОЧЕРЕДЬ", "Склад", "%s ждёт отправки на %s",
              label(f, pid), c->mt[c->pt[t].route[0].mtype].name);

        return;
    }
}

// Станок
static int start_machines(Factory *f)
{
    int changed = 0;

    for (int idx = 0; idx < f->n_mach; idx++)
    {
        Machine *m = &f->mach[idx];
        Store *q = &f->queue[m->type];

        if (m->a.st != IDLE || q->count == 0)
            continue;

        int pid = take(q, 0);
        Part *p = &f->parts[pid];
        const PartType *t = &f->cfg->pt[p->type];

        if (p->step >= t->route_len || t->route[p->step].mtype != m->type)
        {
            violation(f, "%s попала на %s не по порядку маршрута", label(f, pid), m->a.name);
        }

        m->part = pid;
        p->loc = L_MACHINE;
        p->loc_id = idx;
        set_state(f, &m->a, BUSY);
        schedule(f, t->route[p->step].duration, EV_OP_END, idx);
        event(f->now, "ОП.НАЧАЛО", m->a.name, "операция %d/%d над %s, длительность %d%s",
              p->step + 1, t->route_len, label(f, pid), t->route[p->step].duration,
              p->defective ? " (повторная обработка)" : "");
        changed = 1;
    }

    return changed;
}

// Отдать деталь в выходной накопитель; если он полон, станок приостанавливается
static void machine_unload(Factory *f, Machine *m)
{
    if (space(&m->out) > 0)
    {
        put(f, &m->out, m->part);
        event(f->now, "ОЧЕРЕДЬ", m->a.name, "%s -> выходной накопитель (%d/%d)",
              label(f, m->part), m->out.count, m->out.cap);
        m->part = -1;
        set_state(f, &m->a, IDLE);
    }
    else
    {
        set_state(f, &m->a, BLOCKED);
        event(f->now, "ПРОСТОЙ", m->a.name, "выходной накопитель заполнен — станок приостановлен, %s на станке",
              label(f, m->part));
    }
}

static void on_op_end(Factory *f, int idx)
{
    Machine *m = &f->mach[idx];
    Part *p = &f->parts[m->part];
    p->step++;
    m->ops++;
    event(f->now, "ОП.КОНЕЦ", m->a.name, "операция %d/%d над %s завершена",
          p->step, f->cfg->pt[p->type].route_len, label(f, m->part));
    machine_unload(f, m);
}

static int unblock_machines(Factory *f)
{
    int changed = 0;
    for (int idx = 0; idx < f->n_mach; idx++)
    {
        Machine *m = &f->mach[idx];
        if (m->a.st != BLOCKED || space(&m->out) <= 0)
        {
            continue;
        }
        event(f->now, "ПРОСТОЙ", m->a.name, "место освободилось — станок возобновляет работу");
        machine_unload(f, m);
        changed = 1;
    }
    return changed;
}

// Контролёр ОТК
static int start_inspectors(Factory *f)
{
    int changed = 0;
    for (int idx = 0; idx < f->cfg->inspectors; idx++)
    {
        Inspector *in = &f->insp[idx];
        if (in->a.st != IDLE || f->qc_in.count == 0)
        {
            continue;
        }
        in->part = take(&f->qc_in, 0);
        f->parts[in->part].loc = L_INSPECTOR;
        f->parts[in->part].loc_id = idx;
        set_state(f, &in->a, BUSY);
        schedule(f, f->cfg->inspect_time, EV_QC_END, idx);
        event(f->now, "КОНТРОЛЬ", in->a.name, "начало контроля %s", label(f, in->part));
        changed = 1;
    }
    return changed;
}

// Отдать проверенную деталь на выход ОТК; если места нет, контролёр ждёт
static void inspector_unload(Factory *f, Inspector *in)
{
    if (space(&f->qc_out) > 0)
    {
        put(f, &f->qc_out, in->part);
        in->part = -1;
        set_state(f, &in->a, IDLE);
    }
    else
    {
        set_state(f, &in->a, BLOCKED);
        event(f->now, "ПРОСТОЙ", in->a.name, "выход ОТК заполнен — контролёр ждёт, %s у него",
              label(f, in->part));
    }
}

static void on_qc_end(Factory *f, int i)
{
    Inspector *in = &f->insp[i];
    int pid = in->part;
    Part *p = &f->parts[pid];
    const PartType *t = &f->cfg->pt[p->type];
    in->checked++;

    if (rnd() >= t->defect)
    {
        p->passed = 1;
        p->defective = 0;
        event(f->now, "КОНТРОЛЬ", in->a.name, "%s годна — направляется на сборку", label(f, pid));
        inspector_unload(f, in);
        return;
    }

    in->defects++;
    f->defects[p->type]++;
    p->defective = 1;
    event(f->now, "БРАК", in->a.name, "в %s обнаружен брак", label(f, pid));

    if (t->rework_step >= 0 && p->reworks < t->max_rework)
    {
        p->reworks++;
        f->reworks[p->type]++;
        p->step = t->rework_step;
        event(f->now, "ПЕРЕДЕЛКА", in->a.name, "%s возвращается на операцию %d (%s), переделка %d из %d",
              label(f, pid), t->rework_step + 1, f->cfg->mt[t->route[t->rework_step].mtype].name,
              p->reworks, t->max_rework);
        inspector_unload(f, in);
    }
    else
    {
        p->loc = L_SCRAP;
        p->loc_id = -1;
        f->on_line[p->type]--;
        f->scrapped[p->type]++;
        in->part = -1;
        set_state(f, &in->a, IDLE);
        event(f->now, "СПИСАНИЕ", in->a.name, "%s списана: %s", label(f, pid),
              t->rework_step < 0 || t->max_rework == 0 ? "переделка не предусмотрена" : "исчерпан лимит переделок");
    }
}

static int unblock_inspectors(Factory *f)
{
    int changed = 0;
    for (int idx = 0; idx < f->cfg->inspectors; idx++)
    {
        if (f->insp[idx].a.st == BLOCKED && space(&f->qc_out) > 0)
        {
            inspector_unload(f, &f->insp[idx]);
            changed = 1;
        }
    }
    return changed;
}

// Транспортный робот
// Позиция первой детали в s, которую есть куда везти (-1 - нет такой)
static int find_in(Factory *f, Store *s)
{
    for (int idx = 0; idx < s->count; idx++)
    {
        if (space(dest(f, &f->parts[s->items[idx]])) > 0)
        {
            return idx;
        }
    }
    return -1;
}

// Откуда везти. Выход ОТК, выходы приостановленных станков, остальные выходы станков, склад заготовок.
// Линия разгружается раньше, чем на неё подаются новые детали.
static Store *find_job(Factory *f, int *idx)
{
    if ((*idx = find_in(f, &f->qc_out)) >= 0)
    {
        return &f->qc_out;
    }
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < f->n_mach; i++)
        {
            if ((pass == 0) == (f->mach[i].a.st == BLOCKED) && (*idx = find_in(f, &f->mach[i].out)) >= 0)
            {
                return &f->mach[i].out;
            }
        }
    }

    if ((*idx = find_in(f, &f->warehouse)) >= 0)
    {
        return &f->warehouse;
    }

    return NULL;
}

static int dispatch_robots(Factory *f)
{
    int changed = 0;
    for (int i = 0; i < f->cfg->robots; i++)
    {
        Robot *r = &f->rob[i];
        if (r->a.st != IDLE)
        {
            continue;
        }

        int idx;
        Store *from = find_job(f, &idx);

        if (!from)
        {
            break;
        }

        r->part = take(from, idx);
        r->to = dest(f, &f->parts[r->part]);
        r->to->reserved++;
        r->trips++;
        f->parts[r->part].loc = L_ROBOT;
        f->parts[r->part].loc_id = i;
        set_state(f, &r->a, BUSY);
        schedule(f, f->cfg->transport_time, EV_MOVE_END, i);
        event(f->now, "ПЕРЕВОЗКА", r->a.name, "взял %s: %s -> %s", label(f, r->part), from->name, r->to->name);
        changed = 1;
    }
    return changed;
}

static void on_move_end(Factory *f, int i)
{
    Robot *r = &f->rob[i];
    Part *p = &f->parts[r->part];
    r->to->reserved--;
    put(f, r->to, r->part);
    event(f->now, "ПЕРЕВОЗКА", r->a.name, "доставил %s -> %s", label(f, r->part), r->to->name);

    if (r->to->kind == L_KIT)
    {
        f->lead_sum[p->type] += f->now - p->created;
        f->lead_n[p->type]++;
        event(f->now, "КОМПЛЕКТ", "Сборка", "%s поступила в комплект (%d/%d)",
              label(f, r->part), r->to->count, r->to->cap);
    }
    else
    {
        event(f->now, "ОЧЕРЕДЬ", r->a.name, "%s поставлена: %s (%d/%d)",
              label(f, r->part), r->to->name, r->to->count, r->to->cap);
    }

    r->part = -1;
    set_state(f, &r->a, IDLE);
}

// Сборочный пост
static int start_assembly(Factory *f)
{
    const Config *c = f->cfg;
    int changed = 0;
    for (int si = 0; si < c->stations; si++)
    {
        Station *s = &f->st[si];
        if (s->a.st != IDLE)
            continue;
        for (int pr = 0; pr < c->n_pr; pr++)
        {
            if (f->produced[pr] + f->in_asm[pr] >= c->pr[pr].plan)
                continue;
            int complete = 1;

            for (int t = 0; t < c->n_pt; t++)
            {
                if (f->kit[t].count < c->pr[pr].need[t])
                {
                    complete = 0;
                }
            }

            if (!complete)
                continue;

            char list[512];
            int len = 0;
            s->n = 0;
            s->kit_created = f->now;

            for (int t = 0; t < c->n_pt; t++)
            {
                for (int k = 0; k < c->pr[pr].need[t]; k++)
                {
                    int pid = take(&f->kit[t], 0);
                    Part *p = &f->parts[pid];

                    if (!p->passed || p->defective)
                    {
                        violation(f, "в комплект взята непроверенная или бракованная %s", label(f, pid));
                    }

                    p->loc = L_ASSEMBLY;
                    p->loc_id = si;
                    s->parts[s->n++] = pid;
                    if (p->created < s->kit_created)
                    {
                        s->kit_created = p->created;
                    }
                    if (len < (int)sizeof list - 64)
                    {
                        len += snprintf(list + len, sizeof list - (size_t)len, "%s%s", len ? ", " : "", label(f, pid));
                    }
                }
            }

            s->product = pr;
            s->number = ++f->product_seq;
            f->in_asm[pr]++;
            set_state(f, &s->a, BUSY);
            schedule(f, c->assembly_time, EV_ASM_END, si);
            event(f->now, "СБОРКА", s->a.name, "начало сборки %s №%d из комплекта: %s", c->pr[pr].name, s->number, list);
            changed = 1;
            break;
        }
    }

    return changed;
}

static void on_asm_end(Factory *f, int si)
{
    Station *s = &f->st[si];

    for (int k = 0; k < s->n; k++)
    {
        Part *p = &f->parts[s->parts[k]];
        p->loc = L_PRODUCT;
        p->loc_id = -1;
        f->consumed[p->type]++;
        f->on_line[p->type]--;
    }

    f->produced[s->product]++;
    f->in_asm[s->product]--;
    f->cycle_sum += f->now - s->kit_created;
    s->built++;
    event(f->now, "ВЫПУСК", s->a.name, "выпущено изделие %s №%d, по плану %d/%d",
          f->cfg->pr[s->product].name, s->number, f->produced[s->product],
          f->cfg->pr[s->product].plan);
    s->product = -1;
    s->n = 0;
    set_state(f, &s->a, IDLE);
}

// Продвижение модели
void sim_handle(Factory *f, const Event *e)
{
    f->now = e->time;
    f->events++;

    switch (e->type)
    {
    case EV_RELEASE:
        on_release(f);
        break;
    case EV_OP_END:
        on_op_end(f, e->who);
        break;
    case EV_QC_END:
        on_qc_end(f, e->who);
        break;
    case EV_MOVE_END:
        on_move_end(f, e->who);
        break;
    case EV_ASM_END:
        on_asm_end(f, e->who);
        break;
    }

    int changed;
    do
    {
        changed = unblock_machines(f);
        changed |= unblock_inspectors(f);
        changed |= start_machines(f);
        changed |= start_inspectors(f);
        changed |= start_assembly(f);
        changed |= dispatch_robots(f);
    } while (changed);
    plan_release(f);
    check_invariants(f);
}

int sim_plan_done(const Factory *f)
{
    for (int p = 0; p < f->cfg->n_pr; p++)
    {
        if (f->produced[p] < f->cfg->pr[p].plan)
        {
            return 0;
        }
    }

    return 1;
}

void sim_diagnose(Factory *f)
{
    int stuck = 0, n = 0;
    for (int idx = 0; idx < f->n_parts; idx++)
    {
        if (f->parts[idx].loc != L_KIT && f->parts[idx].loc != L_PRODUCT && f->parts[idx].loc != L_SCRAP)
        {
            stuck++;
        }
    }

    for (int t = 0; t < f->cfg->n_pt; t++)
    {
        if (f->raw_left[t] == 0 && f->consumed[t] + f->on_line[t] < f->required[t])
        {
            f->end = END_RAW;
            n += snprintf(f->why + n, sizeof f->why - (size_t)n, "%sзаготовки %s исчерпаны (списано %d)",
                          n ? "; " : "", f->cfg->pt[t].name, f->scrapped[t]);
        }
    }

    if (f->end == END_RAW)
        return;

    if (stuck > 0)
    {
        f->end = END_DEADLOCK;
        n = snprintf(f->why, sizeof f->why, "на линии застряло деталей: %d;", stuck);
        for (int idx = 0; idx < f->n_mach && n < (int)sizeof f->why - 48; idx++)
        {
            if (f->mach[idx].a.st == BLOCKED)
            {
                n += snprintf(f->why + n, sizeof f->why - (size_t)n, " %s приостановлен;", f->mach[idx].a.name);
            }
        }

        for (int idx = 0; idx < f->cfg->inspectors && n < (int)sizeof f->why - 48; idx++)
        {
            if (f->insp[idx].a.st == BLOCKED)
            {
                n += snprintf(f->why + n, sizeof f->why - (size_t)n, " %s ждёт места;", f->insp[idx].a.name);
            }
        }

        return;
    }
    f->end = END_IMPOSSIBLE;
    snprintf(f->why, sizeof f->why, "из оставшихся деталей не собрать полный комплект");
}

void sim_finish(Factory *f)
{
    for (int idx = 0; idx < f->n_mach; idx++)
    {
        set_state(f, &f->mach[idx].a, f->mach[idx].a.st);
    }
    for (int idx = 0; idx < f->cfg->inspectors; idx++)
    {
        set_state(f, &f->insp[idx].a, f->insp[idx].a.st);
    }
    for (int idx = 0; idx < f->cfg->robots; idx++)
    {
        set_state(f, &f->rob[idx].a, f->rob[idx].a.st);
    }
    for (int idx = 0; idx < f->cfg->stations; idx++)
    {
        set_state(f, &f->st[idx].a, f->st[idx].a.st);
    }
}
