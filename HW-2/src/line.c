//-------------------------------------------------------------------------
//  потоки участников сборочной линии
//-------------------------------------------------------------------------
//  Путь детали:
//    склад -(робот)-> очередь станков типа A -> станок A -> выход станка
//    -(робот)-> ... -> вход ОТК -> контролёр -> выход ОТК
//    -(робот)-> комплект (годна) или очередь станка операции k (переделка);
//    списанная деталь выбывает. Полный комплект -> сборочный пост -> изделие.
//
//  Правила синхронизации:
//    - все общие данные меняются только под мьютексом line.lock;
//    - ждать (counter_wait) можно только без line.lock, иначе остальные
//      потоки остановятся;
//    - длительная работа (операция, перевозка, сборка) - тоже без line.lock.
//-------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "model.h"

Line line;               // единственная сборочная линия
static int tick_ms = 50; // длительность единицы модельного времени, мс

static const char *pname(int pid, char *buf)
{
    if (pid < 0)
    {
        return "-";
    }
    snprintf(buf, 48, "%s#%d", line.cfg->pt[line.parts[pid].type].name, pid + 1);
    return buf;
}

// Поработать units единиц модельного времени
static void work(int units, long *busy)
{
    long ms = (long)units * tick_ms;
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);

    if (busy)
    {
        *busy += ms;
    }
}

// Нарушение правила модели
static void violation(const char *text)
{
    line.violations++;
    event("!ИНВАРИАНТ", "Проверка", "%s", text);
}

int stopping(void)
{
    pthread_mutex_lock(&line.lock);
    int s = line.stop;
    pthread_mutex_unlock(&line.lock);
    return s;
}

static void wake_all(void)
{
    for (int k = 0; k < MAX_MACHINES + MAX_ACTORS; k++)
    {
        counter_post(&line.robot_work);
        counter_post(&line.kit_event);
        counter_post(&line.release_wake);
        counter_post(&line.qc_in.ready);
        counter_post(&line.qc_out.free);
        for (int m = 0; m < line.cfg->n_mt; m++)
        {
            counter_post(&line.queue[m].ready);
        }
        for (int idx = 0; idx < line.n_mach; idx++)
        {
            counter_post(&line.mach[idx].out.free);
        }
    }
}

void stop_all(int reason, const char *why)
{
    pthread_mutex_lock(&line.lock);
    int first = !line.stop;
    line.stop = 1;

    if (line.end == END_NONE)
    {
        line.end = reason;
        snprintf(line.why, sizeof line.why, "%s", why);
    }
    pthread_mutex_unlock(&line.lock);
    if (first)
    {
        wake_all();
    }
}

static void store_init(Store *s, const char *name, Loc kind, int cap)
{
    snprintf(s->name, sizeof s->name, "%s", name);
    s->kind = kind;
    s->cap = cap;
    s->count = 0;
    counter_init(&s->free, cap < 0 ? 0 : cap);
    counter_init(&s->ready, 0);
}

// Положить деталь
static void put(Store *s, int pid)
{
    s->items[s->count++] = pid;
    if (s->count > s->max)
    {
        s->max = s->count;
    }
    line.parts[pid].loc = s->kind;
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
    const PartType *t = &line.cfg->pt[p->type];
    if (p->passed)
    {
        return &line.kit[p->type];
    } // годна - в комплект

    if (p->step < t->route_len)
    {
        return &line.queue[t->route[p->step].mtype];
    }

    return &line.qc_in; // маршрут пройден - на ОТК
}

void line_init(const Config *c, int tick)
{
    tick_ms = tick;
    memset(&line, 0, sizeof line);
    line.cfg = c;
    pthread_mutex_init(&line.lock, NULL);
    counter_init(&line.robot_work, 0);
    counter_init(&line.kit_event, 0);
    counter_init(&line.release_wake, 0);

    char nm[48];
    store_init(&line.warehouse, "склад заготовок", L_WAREHOUSE, -1);
    store_init(&line.qc_in, "вход ОТК", L_QC_IN, c->qc_in_cap);
    store_init(&line.qc_out, "выход ОТК", L_QC_OUT, c->qc_out_cap);

    for (int m = 0; m < c->n_mt; m++)
    {
        snprintf(nm, sizeof nm, "очередь %s", c->mt[m].name);
        store_init(&line.queue[m], nm, L_QUEUE, c->queue_cap);

        for (int k = 0; k < c->mt[m].count; k++)
        {
            Machine *mc = &line.mach[line.n_mach];
            mc->id = line.n_mach;
            mc->type = m;
            mc->part = -1;
            snprintf(mc->name, sizeof mc->name, "%s-%d", c->mt[m].name, k + 1);
            snprintf(nm, sizeof nm, "выход %s", mc->name);
            store_init(&mc->out, nm, L_OUTBUF, c->outbuf_cap);
            line.n_mach++;
        }
    }

    for (int t = 0; t < c->n_pt; t++)
    {
        snprintf(nm, sizeof nm, "комплект %s", c->pt[t].name);
        store_init(&line.kit[t], nm, L_KIT, c->kit_cap);
        line.raw_left[t] = c->pt[t].raw;

        for (int p = 0; p < c->n_pr; p++)
        {
            line.required[t] += c->pr[p].plan * c->pr[p].need[t];
        }
    }

    for (int idx = 0; idx < c->inspectors; idx++)
    {
        line.insp[idx].id = idx;
        line.insp[idx].part = -1;
        line.insp[idx].seed = (unsigned)c->seed + 7919u * (unsigned)(idx + 1);
        snprintf(line.insp[idx].name, sizeof line.insp[idx].name, "ОТК-%d", idx + 1);
    }

    for (int idx = 0; idx < c->robots; idx++)
    {
        line.rob[idx].id = idx;
        line.rob[idx].part = -1;
        snprintf(line.rob[idx].name, sizeof line.rob[idx].name, "РОБОТ-%d", idx + 1);
    }

    for (int idx = 0; idx < c->stations; idx++)
    {
        line.st[idx].id = idx;
        snprintf(line.st[idx].name, sizeof line.st[idx].name, "ПОСТ-%d", idx + 1);
    }
}

void line_destroy(void)
{
    Store *all[] = {&line.warehouse, &line.qc_in, &line.qc_out};

    for (int idx = 0; idx < 3; idx++)
    {
        counter_destroy(&all[idx]->free);
        counter_destroy(&all[idx]->ready);
    }

    for (int idx = 0; idx < line.cfg->n_mt; idx++)
    {
        counter_destroy(&line.queue[idx].free);
        counter_destroy(&line.queue[idx].ready);
    }

    for (int idx = 0; idx < line.cfg->n_pt; idx++)
    {
        counter_destroy(&line.kit[idx].free);
        counter_destroy(&line.kit[idx].ready);
    }

    for (int idx = 0; idx < line.n_mach; idx++)
    {
        counter_destroy(&line.mach[idx].out.free);
        counter_destroy(&line.mach[idx].out.ready);
    }

    counter_destroy(&line.robot_work);
    counter_destroy(&line.kit_event);
    counter_destroy(&line.release_wake);
    pthread_mutex_destroy(&line.lock);
}

void *warehouse_thread(void *arg)
{
    (void)arg;
    const Config *c = line.cfg;
    char pn[48];

    for (;;)
    {
        if (c->release_interval > 0)
        {
            work(c->release_interval, NULL);
        }
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        }
        int t = -1; // какой тип выпускать

        for (int k = 0; k < c->n_pt && !line.soft_stop; k++)
        {
            int tt = (line.release_rr + k) % c->n_pt;
            if (line.raw_left[tt] > 0 && line.consumed[tt] + line.on_line[tt] < line.required[tt])
            {
                t = tt;
                break;
            }
        }

        if (t < 0)
        { // выпускать нечего, ждём списания
            pthread_mutex_unlock(&line.lock);
            counter_wait(&line.release_wake);
            continue;
        }

        line.release_rr = t + 1;
        int pid = line.n_parts++;
        line.parts[pid] = (Part){t, 0, 0, 0, now_t(), L_WAREHOUSE};
        line.raw_left[t]--;
        line.on_line[t]++;
        line.created[t]++;
        put(&line.warehouse, pid);
        event("СОЗДАНИЕ", "Склад", "создана деталь %s, осталось заготовок %d", pname(pid, pn), line.raw_left[t]);
        event("ОЧЕРЕДЬ", "Склад", "%s ждёт отправки на %s", pn, c->mt[c->pt[t].route[0].mtype].name);
        pthread_mutex_unlock(&line.lock);
        counter_post(&line.robot_work);
    }
    return NULL;
}

void *machine_thread(void *arg)
{
    Machine *m = arg;
    Store *q = &line.queue[m->type];
    char pn[48];

    for (;;)
    {
        counter_wait(&q->ready); // ждём деталь в очереди
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        }
        if (q->count == 0)
        {
            pthread_mutex_unlock(&line.lock);
            continue;
        }

        int pid = take(q, 0);
        Part *p = &line.parts[pid];
        const PartType *t = &line.cfg->pt[p->type];
        line.checks += 2;

        if (p->step >= t->route_len || t->route[p->step].mtype != m->type)
        {
            violation("деталь попала на станок не по порядку маршрута");
        }
        if (m->part != -1)
        {
            violation("станок уже обрабатывает деталь");
        }

        m->part = pid;
        p->loc = L_MACHINE;
        int dur = t->route[p->step].duration;
        event("ОП.НАЧАЛО", m->name, "операция %d/%d над %s, длительность %d", p->step + 1, t->route_len, pname(pid, pn), dur);
        pthread_mutex_unlock(&line.lock);
        counter_post(&q->free); // в очереди освободилось место
        counter_post(&line.robot_work);

        work(dur, &m->busy_ms); // выполняем операцию

        pthread_mutex_lock(&line.lock);
        p->step++; // маршрут продвигается только здесь
        m->ops++;
        event("ОП.КОНЕЦ", m->name, "операция %d/%d над %s завершена", p->step, t->route_len, pn);
        pthread_mutex_unlock(&line.lock);

        if (counter_trywait(&m->out.free) != 0)
        { // выходной накопитель полон
            pthread_mutex_lock(&line.lock);
            event("ПРОСТОЙ", m->name, "выходной накопитель заполнен - станок приостановлен, %s на станке", pn);
            pthread_mutex_unlock(&line.lock);
            long t0 = now_ms();
            counter_wait(&m->out.free);
            m->blocked_ms += now_ms() - t0;

            if (stopping())
            {
                break;
            }
            pthread_mutex_lock(&line.lock);
            event("ПРОСТОЙ", m->name, "место освободилось - станок возобновляет работу");
            pthread_mutex_unlock(&line.lock);
        }
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        }
        put(&m->out, pid);
        m->part = -1;
        event("ОЧЕРЕДЬ", m->name, "%s -> выходной накопитель (%d/%d)", pn, m->out.count, m->out.cap);
        pthread_mutex_unlock(&line.lock);
        counter_post(&line.robot_work); // деталь можно увозить
    }
    return NULL;
}

void *inspector_thread(void *arg)
{
    Inspector *in = arg;
    char pn[48];

    for (;;)
    {
        counter_wait(&line.qc_in.ready);
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        }

        if (line.qc_in.count == 0)
        {
            pthread_mutex_unlock(&line.lock);
            continue;
        }

        int pid = take(&line.qc_in, 0);
        Part *p = &line.parts[pid];
        const PartType *t = &line.cfg->pt[p->type];
        in->part = pid;
        in->checked++;
        p->loc = L_INSPECTOR;
        event("КОНТРОЛЬ", in->name, "начало контроля %s", pname(pid, pn));
        pthread_mutex_unlock(&line.lock);
        counter_post(&line.qc_in.free);
        counter_post(&line.robot_work);

        work(line.cfg->inspect_time, &in->busy_ms);
        int bad = rand_r(&in->seed) / ((double)RAND_MAX + 1.0) < t->defect;

        pthread_mutex_lock(&line.lock);
        if (!bad)
        {
            p->passed = 1;
            event("КОНТРОЛЬ", in->name, "%s годна - направляется на сборку", pn);
        }
        else
        {
            in->defects++;
            line.defects[p->type]++;
            event("БРАК", in->name, "в %s обнаружен брак", pn);

            if (t->rework_step >= 0 && p->reworks < t->max_rework)
            {
                p->reworks++;
                line.reworks[p->type]++;
                p->step = t->rework_step;
                event("ПЕРЕДЕЛКА", in->name, "%s возвращается на операцию %d (%s), переделка %d из %d", pn,
                      t->rework_step + 1, line.cfg->mt[t->route[t->rework_step].mtype].name, p->reworks, t->max_rework);
            }
            else
            {
                p->loc = L_SCRAP;
                line.on_line[p->type]--;
                line.scrapped[p->type]++;
                in->part = -1;
                event("СПИСАНИЕ", in->name, "%s списана: %s", pn, t->rework_step < 0 || t->max_rework == 0 ? "переделка не предусмотрена" : "исчерпан лимит переделок");
                pthread_mutex_unlock(&line.lock);
                counter_post(&line.release_wake);
                continue;
            }
        }
        pthread_mutex_unlock(&line.lock);

        if (counter_trywait(&line.qc_out.free) != 0)
        {
            pthread_mutex_lock(&line.lock);
            event("ПРОСТОЙ", in->name, "выход ОТК заполнен - контролёр ждёт, %s у него", pn);
            pthread_mutex_unlock(&line.lock);
            long t0 = now_ms();
            counter_wait(&line.qc_out.free);
            in->blocked_ms += now_ms() - t0;

            if (stopping())
            {
                break;
            }
        }
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        } // деталь остаётся у контролёра

        put(&line.qc_out, pid);
        in->part = -1;
        pthread_mutex_unlock(&line.lock);
        counter_post(&line.robot_work);
    }

    return NULL;
}

// Найти в накопителе s деталь, для которой удалось забронировать место
static int try_store(Store *s, int *idx, Store **to)
{
    for (int i = 0; i < s->count; i++)
    {
        Store *d = dest(&line.parts[s->items[i]]);
        if (counter_trywait(&d->free) == 0)
        { // место забронировано
            *idx = i;
            *to = d;
            return 1;
        }
    }
    return 0;
}

// Откуда везти: сначала выход ОТК, потом выходы станков, потом склад
// (сначала разгружаем линию, потом подаём на неё новые детали)
static Store *find_job(int *idx, Store **to)
{
    if (try_store(&line.qc_out, idx, to))
    {
        return &line.qc_out;
    }

    for (int i = 0; i < line.n_mach; i++)
    {
        if (try_store(&line.mach[i].out, idx, to))
        {
            return &line.mach[i].out;
        }
    }

    if (try_store(&line.warehouse, idx, to))
    {
        return &line.warehouse;
    }

    return NULL;
}

void *robot_thread(void *arg)
{
    Robot *r = arg;
    char pn[48];

    for (;;)
    {
        counter_wait(&line.robot_work); // ждём, пока что-то изменится
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        }
        int idx;
        Store *to = NULL;
        Store *from = find_job(&idx, &to);

        if (!from)
        {
            pthread_mutex_unlock(&line.lock);
            continue;
        }

        int pid = take(from, idx);
        r->part = pid;
        r->trips++;
        line.parts[pid].loc = L_ROBOT;
        event("ПЕРЕВОЗКА", r->name, "взял %s: %s -> %s", pname(pid, pn), from->name, to->name);
        pthread_mutex_unlock(&line.lock);

        if (from->cap >= 0)
        {
            counter_post(&from->free); // у источника освободилось место
        }
        counter_post(&line.robot_work); // вдруг работа есть и для другого робота

        work(line.cfg->transport_time, &r->busy_ms);

        pthread_mutex_lock(&line.lock);
        put(to, pid);
        r->part = -1;
        event("ПЕРЕВОЗКА", r->name, "доставил %s -> %s", pn, to->name);

        if (to->kind == L_KIT)
        {
            line.lead_sum[line.parts[pid].type] += now_t() - line.parts[pid].born;
            line.lead_n[line.parts[pid].type]++;
        }
        if (to->kind == L_KIT)
        {
            event("КОМПЛЕКТ", "Сборка", "%s поступила в комплект (%d/%d)", pn, to->count, to->cap);
        }
        else
        {
            event("ОЧЕРЕДЬ", r->name, "%s поставлена: %s (%d/%d)", pn, to->name, to->count, to->cap);
        }
        pthread_mutex_unlock(&line.lock);
        if (to->kind == L_KIT)
        {
            counter_post(&line.kit_event); // посты проверят комплект
        }
        else
        {
            counter_post(&to->ready); // станок или ОТК может брать
        }
    }

    return NULL;
}

void *station_thread(void *arg)
{
    Station *s = arg;
    const Config *c = line.cfg;
    char pn[48];
    for (;;)
    {
        counter_wait(&line.kit_event); // ждём изменений в комплектах
        pthread_mutex_lock(&line.lock);

        if (line.stop)
        {
            pthread_mutex_unlock(&line.lock);
            break;
        }

        int pr = -1; // какое изделие можно собрать
        for (int i = 0; i < c->n_pr && pr < 0; i++)
        {
            if (line.produced[i] + line.in_asm[i] >= c->pr[i].plan)
            {
                continue;
            }
            int complete = 1;
            for (int t = 0; t < c->n_pt; t++)
            {
                if (line.kit[t].count < c->pr[i].need[t])
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
            pthread_mutex_unlock(&line.lock);
            continue;
        } // полного комплекта нет

        char list[512] = "";
        int len = 0;
        s->n = 0;
        s->kit_born = now_t();
        for (int t = 0; t < c->n_pt; t++) // забираем весь комплект разом
        {
            for (int k = 0; k < c->pr[pr].need[t]; k++)
            {
                int pid = take(&line.kit[t], 0);
                if (line.parts[pid].born < s->kit_born)
                {
                    s->kit_born = line.parts[pid].born;
                }

                line.checks++;
                if (!line.parts[pid].passed)
                {
                    violation("в комплект попала непроверенная деталь");
                }

                line.parts[pid].loc = L_ASSEMBLY;
                s->parts[s->n++] = pid;
                if (len < (int)sizeof list - 64)
                {
                    len += snprintf(list + len, sizeof list - (size_t)len, "%s%s", len ? ", " : "", pname(pid, pn));
                }
            }
        }

        line.in_asm[pr]++;
        int number = ++line.product_no;
        event("СБОРКА", s->name, "начало сборки %s №%d из комплекта: %s", c->pr[pr].name, number, list);
        pthread_mutex_unlock(&line.lock);
        for (int k = 0; k < s->n; k++) // в комплектах освободились места
        {
            counter_post(&line.kit[line.parts[s->parts[k]].type].free);
        }

        counter_post(&line.robot_work);
        counter_post(&line.kit_event); // другой пост тоже может проверить

        work(c->assembly_time, &s->busy_ms); // собираем

        pthread_mutex_lock(&line.lock);

        for (int k = 0; k < s->n; k++)
        {
            Part *p = &line.parts[s->parts[k]];
            p->loc = L_PRODUCT;
            line.consumed[p->type]++;
            line.on_line[p->type]--;
        }

        s->n = 0;
        s->built++;
        line.produced[pr]++;
        line.in_asm[pr]--;
        line.cycle_sum += now_t() - s->kit_born;
        event("ВЫПУСК", s->name, "выпущено изделие %s №%d, по плану %d/%d", c->pr[pr].name, number, line.produced[pr], c->pr[pr].plan);
        int done = 1;

        for (int i = 0; i < c->n_pr; i++)
        {
            if (line.produced[i] < c->pr[i].plan)
            {
                done = 0;
            }
        }
        pthread_mutex_unlock(&line.lock);

        if (done)
        {
            stop_all(END_PLAN, "все изделия по плану выпущены");
        }
    }
    return NULL;
}

void diagnose(void)
{
    const Config *c = line.cfg;
    int stuck = 0, n = 0;
    for (int i = 0; i < line.n_parts; i++)
    {
        Loc l = line.parts[i].loc;
        if (l != L_KIT && l != L_PRODUCT && l != L_SCRAP)
        {
            stuck++;
        }
    }

    for (int t = 0; t < c->n_pt; t++)
    {
        if (line.raw_left[t] == 0 && line.consumed[t] + line.on_line[t] < line.required[t])
        {
            line.end = END_RAW;
            n += snprintf(line.why + n, sizeof line.why - (size_t)n, "%sзаготовки %s исчерпаны (списано %d)",
                          n ? "; " : "", c->pt[t].name, line.scrapped[t]);
        }
    }

    if (line.end == END_RAW)
    {
        return;
    }

    if (stuck > 0)
    { // детали остались, но не двигаются
        line.end = END_DEADLOCK;
        n = snprintf(line.why, sizeof line.why, "на линии застряло деталей: %d;", stuck);
        for (int i = 0; i < line.n_mach && n < (int)sizeof line.why - 48; i++)
        {
            if (line.mach[i].part >= 0)
            {
                n += snprintf(line.why + n, sizeof line.why - (size_t)n, " %s приостановлен;", line.mach[i].name);
            }
        }

        for (int i = 0; i < c->inspectors && n < (int)sizeof line.why - 48; i++)
        {
            if (line.insp[i].part >= 0)
            {
                n += snprintf(line.why + n, sizeof line.why - (size_t)n, " %s ждёт места;", line.insp[i].name);
            }
        }
        return;
    }
    line.end = END_IMPOSSIBLE;
    snprintf(line.why, sizeof line.why, "из оставшихся деталей не собрать полный комплект");
}

// После остановки всех потоков: каждая деталь должна быть ровно в одном месте
int check_final(void)
{
    static int seen[MAX_PARTS];
    memset(seen, 0, sizeof seen);
    Store *st[3 + 2 * MAX_TYPES + MAX_MACHINES];
    int ns = 0;
    st[ns++] = &line.warehouse;
    st[ns++] = &line.qc_in;
    st[ns++] = &line.qc_out;

    for (int i = 0; i < line.cfg->n_mt; i++)
    {
        st[ns++] = &line.queue[i];
    }
    for (int i = 0; i < line.cfg->n_pt; i++)
    {
        st[ns++] = &line.kit[i];
    }
    for (int i = 0; i < line.n_mach; i++)
    {
        st[ns++] = &line.mach[i].out;
    }

    for (int k = 0; k < ns; k++)
    {
        if (st[k]->cap >= 0 && st[k]->count > st[k]->cap)
        {
            violation("накопитель переполнен");
        }
        for (int i = 0; i < st[k]->count; i++)
        {
            int pid = st[k]->items[i];
            seen[pid]++;
            if (line.parts[pid].loc != st[k]->kind)
            {
                violation("у детали записано не то место");
            }
            if (st[k]->kind == L_KIT && !line.parts[pid].passed)
            {
                violation("в комплекте непроверенная деталь");
            }
        }
    }

    for (int i = 0; i < line.n_mach; i++)
    {
        if (line.mach[i].part >= 0)
        {
            seen[line.mach[i].part]++;
        }
    }

    for (int i = 0; i < line.cfg->inspectors; i++)
    {
        if (line.insp[i].part >= 0)
        {
            seen[line.insp[i].part]++;
        }
    }

    for (int i = 0; i < line.cfg->robots; i++)
    {
        if (line.rob[i].part >= 0)
        {
            seen[line.rob[i].part]++;
        }
    }

    for (int i = 0; i < line.cfg->stations; i++)
    {
        for (int k = 0; k < line.st[i].n; k++)
        {
            seen[line.st[i].parts[k]]++;
        }
    }

    line.checks += ns + line.n_parts;
    for (int i = 0; i < line.n_parts; i++)
    {
        int gone = line.parts[i].loc == L_PRODUCT || line.parts[i].loc == L_SCRAP;
        if (seen[i] != (gone ? 0 : 1))
        {
            violation("деталь найдена не в одном месте");
        }
    }
    return line.violations;
}

// Текущее состояние линии
void print_status(void)
{
    const Config *c = line.cfg;
    char buf[2048], pn[48];
    int n = 0;
    pthread_mutex_lock(&line.lock);
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\n--- Состояние линии, t=%ld ---\nВыпуск:", now_t());

    for (int i = 0; i < c->n_pr; i++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s %d/%d", c->pr[i].name, line.produced[i], c->pr[i].plan);
    }

    n += snprintf(buf + n, sizeof buf - (size_t)n, "\nСклад: %d, вход ОТК: %d/%d, выход ОТК: %d/%d\nСтанки:",
                  line.warehouse.count, line.qc_in.count, line.qc_in.cap, line.qc_out.count, line.qc_out.cap);

    for (int i = 0; i < line.n_mach && n < (int)sizeof buf - 100; i++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s[%s]", line.mach[i].name, pname(line.mach[i].part, pn));
    }

    n += snprintf(buf + n, sizeof buf - (size_t)n, "\nРоботы:");

    for (int i = 0; i < c->robots && n < (int)sizeof buf - 100; i++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s[%s]", line.rob[i].name, pname(line.rob[i].part, pn));
    }

    n += snprintf(buf + n, sizeof buf - (size_t)n, "\nКомплекты:");
    
    for (int t = 0; t < c->n_pt && n < (int)sizeof buf - 100; t++)
    {
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s %d/%d", c->pt[t].name, line.kit[t].count, line.kit[t].cap);
    }

    snprintf(buf + n, sizeof buf - (size_t)n, "\n---\n");
    pthread_mutex_unlock(&line.lock);
    say("%s", buf);
}