#ifndef MODEL_H
#define MODEL_H
//-------------------------------------------------------------------------
//  состояние сборочной линии у TCP-сервера
//-------------------------------------------------------------------------
//  Каждый участник с поведением - отдельный процесс:
//    склад, станки, контролёры ОТК, роботы, сборочные посты.
//  Сервер выполняет операции line_op() по одной. Рабочие процессы
//  получают задания и сообщают об их выполнении через TCP.
//-------------------------------------------------------------------------

#include "config.h"

#define MAX_PARTS 1024 // деталей за один прогон
#define MAX_STORES (3 + 2 * MAX_TYPES + MAX_MACHINES)

// Роли участников
enum
{
    R_WAREHOUSE,
    R_MACHINE,
    R_INSPECTOR,
    R_ROBOT,
    R_STATION,
    R_COUNT
};

// Операции над линией (что процесс просит сделать)
enum
{
    OP_W_RELEASE, // склад: выпустить заготовку
    OP_M_TAKE,
    OP_M_DONE,
    OP_M_RESUME, // станок
    OP_I_TAKE,
    OP_I_DECIDE,
    OP_I_PUT, // контролёр
    OP_R_TAKE,
    OP_R_DELIVER, // робот
    OP_S_TAKE,
    OP_S_DONE // сборочный пост
};

// Где находится деталь
typedef enum Loc
{
    L_WAREHOUSE,
    L_QUEUE,
    L_MACHINE,
    L_OUTBUF,
    L_ROBOT,
    L_QC_IN,
    L_INSPECTOR,
    L_QC_OUT,
    L_KIT,
    L_ASSEMBLY,
    L_PRODUCT,
    L_SCRAP
} Loc;

typedef struct Part
{                // Деталь
    int type;    // тип детали (индекс в cfg.pt)
    int step;    // номер следующей операции маршрута (с 0)
    int reworks; // сколько раз переделывалась
    int passed;  // 1 - прошла контроль
    long born;   // когда создана (модельное время)
    Loc loc;     // где находится
} Part;

typedef struct Store
{ // Накопитель
    char name[48];
    Loc kind;             // какой loc у деталей внутри
    int items[MAX_PARTS]; // номера деталей по порядку поступления
    int count, cap, max;  // сейчас, вместимость (-1 - без ограничения), наибольшее
    int reserved;         // места, уже занятые деталями в пути
} Store;

typedef struct Actor
{ // Общее у всех участников
    char name[32];
    int part;     // деталь у участника (-1 - нет)
    int done;     // операций / проверок / рейсов / изделий
    int dest;     // робот: куда везёт деталь (номер накопителя)
    long from_ms; // когда начата текущая работа (для статистики)
    long busy_ms, blocked_ms;
} Actor;

typedef struct Machine
{ // Станок
    Actor a;
    int type;  // тип станка
    Store out; // выходной накопитель
} Machine;

typedef struct Inspector
{ // Контролёр ОТК
    Actor a;
    int defects;
    unsigned seed; // своё состояние генератора
} Inspector;

typedef struct Station
{ // Сборочный пост
    Actor a;
    int parts[MAX_KIT], n; // детали собираемого комплекта
    int product, number;   // что собирает и номер изделия
    long kit_born;         // когда создана самая ранняя деталь комплекта
} Station;

// Причины завершения
enum
{
    END_NONE,
    END_PLAN,
    END_RAW,
    END_DEADLOCK,
    END_IMPOSSIBLE,
    END_TIMEOUT,
    END_SIGNAL
};

typedef struct Line
{
    Config cfg;       // копия параметров
    int tick_ms;      // мс в единице модельного времени
    long start_clock; // момент начала моделирования
    long events;      // сколько событий выведено

    Part parts[MAX_PARTS];
    int n_parts;
    Store warehouse, qc_in, qc_out;
    Store queue[MAX_TYPES]; // очереди к станкам по типам станков
    Store kit[MAX_TYPES];   // комплектующие по типам деталей
    Actor wh;               // склад
    Machine mach[MAX_MACHINES];
    int n_mach;
    Inspector insp[MAX_ACTORS];
    Actor rob[MAX_ACTORS];
    Station st[MAX_ACTORS];
    int expected, alive; // сколько процессов должно быть / подключено сейчас

    int raw_left[MAX_TYPES];
    long required[MAX_TYPES];
    int on_line[MAX_TYPES], consumed[MAX_TYPES];
    int created[MAX_TYPES], scrapped[MAX_TYPES], defects[MAX_TYPES], reworks[MAX_TYPES];
    int produced[MAX_PRODUCTS], in_asm[MAX_PRODUCTS], product_no;
    int release_rr; // чья очередь выпускать заготовку
    long cycle_sum; // сумма циклов изделий
    long lead_sum[MAX_TYPES];
    int lead_n[MAX_TYPES]; // путь детали до комплекта
    long checks;           // выполнено проверок правил
    int violations;        // найдено нарушений правил

    int started;   // 1 - моделирование началось
    int stop;      // 1 - всем процессам пора завершаться
    int end;       // причина завершения
    int signal_no; // сигнал, остановивший программу
    char why[256];
} Line;

extern Line *L; // линия на сервере

void line_init(Line *l, const Config *c, int tick_ms);
int line_op(int op, int who);               // выполнить операцию
int line_expected(int role);                // сколько процессов этой роли
void line_start(void);                      // все подключились - начинаем
void stop_all(int reason, const char *why); // остановить линию
void diagnose(void);                        // почему линия встала
int store_room(const Store *s);             // есть место с учётом брони
const char *actor_name(int role, int who);
void check_final(void);  // итоговая проверка правил
void print_status(void); // текущее состояние для наблюдателя
void report(void);       // итоги
int exit_code(void);     // код завершения по причине

#endif
