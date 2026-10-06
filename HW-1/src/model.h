#ifndef MODEL_H
#define MODEL_H
//-----------------------------------------------------------------------
//  Участники модели сборочной линии
//-----------------------------------------------------------------------
//  Деталь, станок, контролёр, робот и сборочный пост - отдельные типы
//  данных; их поведение - функции в sim.c. Накопитель (Store) - очередь
//  деталей ограниченной вместимости. Всё вместе - структура Factory.
//-----------------------------------------------------------------------

#include "config.h"

// Где находится деталь
typedef enum Loc
{
    L_WAREHOUSE, // склад заготовок
    L_QUEUE,     // входная очередь станков одного типа
    L_MACHINE,   // на станке
    L_OUTBUF,    // выходной накопитель станка
    L_ROBOT,     // у робота
    L_QC_IN,     // вход ОТК
    L_INSPECTOR, // у контролёра
    L_QC_OUT,    // выход ОТК
    L_KIT,       // накопитель комплектующих у сборки
    L_ASSEMBLY,  // на сборочном посту
    L_PRODUCT,   // в готовом изделии (конец пути)
    L_SCRAP      // списана (конец пути)
} Loc;

typedef struct Part
{                  // Деталь
    int type;      // Тип детали (индекс в Config.pt)
    int step;      // Индекс следующей операции; = route_len - маршрут пройден
    int reworks;   // Сколько раз переделывалась
    int defective; // Признана браком и ещё не прошла повторный контроль
    int passed;    // Прошла контроль после последней обработки
    Loc loc;       // Где находится
    int loc_id;    // Номер места (станка, робота, типа...) или -1
    long created;  // Время создания
    int flagged;   // О нарушении по этой детали уже сообщено
} Part;

typedef struct Store
{ // Накопитель
    char name[64];
    Loc kind;         // Каким должен быть loc у деталей внутри
    int owner;        // Каким должен быть loc_id у деталей внутри
    int *items;       // Номера деталей в порядке поступления
    int count, alloc; // Сколько лежит / размер массива items
    int cap;          // Вместимость (-1 - без ограничения)
    int reserved;     // Мест забронировано едущими роботами
    int max;          // Максимальное заполнение (статистика)
} Store;

typedef enum State
{
    IDLE,
    BUSY,
    BLOCKED
} State; // BLOCKED - некуда отдать результат

typedef struct Actor
{ // Общая часть активных участников
    char name[40];
    State st;
    long since;         // Когда вошёл в текущее состояние
    long busy, blocked; // Накопленное время работы и блокировки
} Actor;

typedef struct Machine
{ // Станок
    Actor a;
    int type;  // Тип станка
    int part;  // Деталь на станке (-1 - нет). Одна - инвариант
    int ops;   // Выполнено операций
    Store out; // Выходной накопитель
} Machine;

typedef struct Inspector
{ // Контролёр ОТК
    Actor a;
    int part;
    int checked, defects;
} Inspector;

typedef struct Robot
{ // Транспортный робот
    Actor a;
    int part;
    Store *to; // Куда везёт
    int trips;
} Robot;

typedef struct Station
{ // Сборочный пост
    Actor a;
    int product;           // Что собирает (-1 - ничего)
    int number;            // Номер собираемого экземпляра
    int parts[MAX_KIT], n; // Детали комплекта
    long kit_created;      // Время создания самой ранней детали комплекта
    int built;
} Station;

typedef enum EvType
{
    EV_RELEASE,
    EV_OP_END,
    EV_QC_END,
    EV_MOVE_END,
    EV_ASM_END
} EvType;

typedef struct Event
{                   // Запись календаря событий
    long time, seq; // Когда наступит; порядковый номер (при равном времени)
    EvType type;
    int who; // Номер участника
} Event;

// У каждого участника не больше одного будущего события, плюс запуск заготовки
#define MAX_EVENTS (MAX_MACHINES + 3 * MAX_ACTORS + 1)

typedef enum EndReason
{
    END_NONE,
    END_PLAN,
    END_RAW,
    END_DEADLOCK,
    END_IMPOSSIBLE,
    END_TIMEOUT,
    END_SIGNAL
} EndReason;

typedef struct Factory
{ // Завод
    const Config *cfg;
    long now, events, seq; // Модельное время, счётчики
    Event cal[MAX_EVENTS];
    int n_cal; // Календарь событий
    Part *parts;
    int n_parts, cap_parts;

    Store warehouse, qc_in, qc_out;
    Store queue[MAX_TYPES]; // Очереди по типам станков
    Store kit[MAX_TYPES];   // Комплектующие по типам деталей
    Machine mach[MAX_MACHINES];
    int n_mach;
    Inspector insp[MAX_ACTORS];
    Robot rob[MAX_ACTORS];
    Station st[MAX_ACTORS];

    // По типам деталей
    int raw_left[MAX_TYPES], required[MAX_TYPES]; // Осталось заготовок / нужно по плану
    int on_line[MAX_TYPES], consumed[MAX_TYPES];  // На линии / израсходовано в изделия
    int created[MAX_TYPES], scrapped[MAX_TYPES], defects[MAX_TYPES], reworks[MAX_TYPES];
    long lead_sum[MAX_TYPES];
    int lead_n[MAX_TYPES]; // Время пути до комплекта
    // По изделиям
    int produced[MAX_PRODUCTS], in_asm[MAX_PRODUCTS], product_seq;
    long cycle_sum;

    int release_rr, release_pending; // Запуск заготовок: чей черёд, запланирован ли
    long checks, violations;         // Проверки инвариантов
    EndReason end;
    char why[256];
} Factory;

// sim.c
void sim_init(Factory *f, const Config *c);
void sim_free(Factory *f);
int sim_next(Factory *f, Event *e);          // Взять ближайшее событие; 0 - календарь пуст
void sim_handle(Factory *f, const Event *e); // Обработать событие и всё, что оно сделало возможным
int sim_plan_done(const Factory *f);
void sim_diagnose(Factory *f); // Почему события кончились, а план не выполнен
void sim_finish(Factory *f);   // Закрыть учёт времени участников
const char *label(const Factory *f, int pid);

// check.c
void check_invariants(Factory *f);
void violation(Factory *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
