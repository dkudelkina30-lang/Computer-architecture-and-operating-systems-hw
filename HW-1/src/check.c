//  Проверка инвариантов после каждого события
//---------------------------------------------------------------------------
//  1) Станок обрабатывает не более одной детали.
//  2) Деталь находится ровно в одном месте: на стадии или у робота.
//  3) Операция выполняется после всех предыдущих операций маршрута.
//  4) Забракованная (непроверенная) деталь не попадает в комплект и изделие.
//  5) Сборка идёт только из полного комплекта.
//
//  Обходим места, где может лежать деталь, считаем, сколько раз
//  встретилась каждая, и сверяем.
//----------------------------------------------------------------------------

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "log.h"
#include "model.h"

void violation(Factory *f, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    f->violations++;
    event(f->now, "!Инвариант", "Проверка", "%s", buf);
}

static void check_store(Factory *f, const Store *s, int *seen)
{
    if (s->cap >= 0 && s->count + s->reserved > s->cap)
    {
        violation(f, "%s переполнен", s->name);
    }
    for (int idx = 0; idx < s->count; idx++)
    {
        const Part *p = &f->parts[s->items[idx]];
        seen[s->items[idx]]++;
        if (p->loc != s->kind || p->loc_id != s->owner)
        {
            violation(f, "%s лежит в '%s', но записано другое место", label(f, s->items[idx]), s->name);
        }
    }
}

// Станок, контролёр, робот: пуст, держит не больше одной детали
static void check_holder(Factory *f, const Actor *a, int pid, Loc kind, int id, int *seen)
{
    if ((a->st == IDLE) != (pid < 0))
    {
        violation(f, "%s: состояние не соответствует наличию детали", a->name);
    }
    if (pid < 0)
    {
        return;
    }
    seen[pid]++;
    if (f->parts[pid].loc != kind || f->parts[pid].loc_id != id)
    {
        violation(f, "%s у %s, но записано другое место", label(f, pid), a->name);
    }
}

void check_invariants(Factory *f)
{
    const Config *c = f->cfg;
    int *seen = calloc((size_t)f->n_parts + 1, sizeof(int));
    if (!seen)
    {
        return;
    }
    f->checks++;

    check_store(f, &f->warehouse, seen);
    check_store(f, &f->qc_in, seen);
    check_store(f, &f->qc_out, seen);
    for (int m = 0; m < c->n_mt; m++)
    {
        check_store(f, &f->queue[m], seen);
        for (int idx = 0; idx < f->queue[m].count; idx++)
        {
            const Part *p = &f->parts[f->queue[m].items[idx]];
            const PartType *t = &c->pt[p->type];
            if (p->step >= t->route_len || t->route[p->step].mtype != m)
            {
                violation(f, "%s в очереди %s не по маршруту", label(f, f->queue[m].items[idx]), c->mt[m].name);
            }
        }
    }
    for (int t = 0; t < c->n_pt; t++)
    {
        check_store(f, &f->kit[t], seen);
    }

    for (int idx = 0; idx < f->n_mach; idx++)
    {
        check_holder(f, &f->mach[idx].a, f->mach[idx].part, L_MACHINE, idx, seen);
        check_store(f, &f->mach[idx].out, seen);
    }

    for (int idx = 0; idx < c->inspectors; idx++)
    {
        check_holder(f, &f->insp[idx].a, f->insp[idx].part, L_INSPECTOR, idx, seen);
    }

    for (int idx = 0; idx < c->robots; idx++)
    {
        check_holder(f, &f->rob[idx].a, f->rob[idx].part, L_ROBOT, idx, seen);
    }

    for (int si = 0; si < c->stations; si++)
    {
        const Station *s = &f->st[si];
        int cnt[MAX_TYPES] = {0};
        for (int k = 0; k < s->n; k++)
        {
            seen[s->parts[k]]++;
            cnt[f->parts[s->parts[k]].type]++;
        }
        for (int t = 0; t < c->n_pt; t++)
        {
            if (cnt[t] != (s->product < 0 ? 0 : c->pr[s->product].need[t]))
            {
                violation(f, "%s: комплект неполный или лишний (%s)", s->a.name, c->pt[t].name);
            }
        }
    }

    for (int idx = 0; idx < f->n_parts; idx++)
    {
        Part *p = &f->parts[idx];
        int gone = p->loc == L_PRODUCT || p->loc == L_SCRAP;
        if (seen[idx] != (gone ? 0 : 1))
        {
            violation(f, "%s найдена в %d местах", label(f, idx), seen[idx]);
        }
        if ((p->loc == L_KIT || p->loc == L_ASSEMBLY || p->loc == L_PRODUCT) && (!p->passed || p->defective) && !p->flagged)
        {
            p->flagged = 1;
            violation(f, "бракованная или непроверенная %s в комплекте/изделии", label(f, idx));
        }
    }
    free(seen);
}
