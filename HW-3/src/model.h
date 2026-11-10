#ifndef MODEL_H
#define MODEL_H
//-------------------------------------------------------------------------
//  сборочная линия, моделируемая взаимодействующими процессами
//-------------------------------------------------------------------------
//  Каждый участник с поведением - отдельный процесс:
//    склад, станки, контролёры ОТК, роботы, сборочные посты.
//  Состояние линии (детали, накопители, статистика) - структура Line.
//  В программах на разделяемой памяти Line лежит в разделяемой памяти,
//  а в программе на очередях сообщений - только у процесса-менеджера.
//
//  Любое изменение линии - это «операция» (OP_...), которую выполняет
//  функция line_op() целиком и без перерыва:
//    - в разделяемой памяти - под семафором-замком S_LOCK;
//    - в версии с очередями - менеджер выполняет операции по одной.
//  Ожидание (детали, свободного места, работы) - на семафорах, номера
//  которых хранятся в Line
//-------------------------------------------------------------------------

#include "config.h"

#define MAX_PARTS 1024 // деталей за один прогон
#define MAX_STORES (3 + 2 * MAX_TYPES + MAX_MACHINES)
#define MAX_SEMS (5 + 2 * MAX_STORES) // семафоров не больше

// Семафоры с постоянными номерами, дальше - по два на каждый накопитель
enum
{
    S_LOCK,    // замок: защищает Line (начальное значение 1)
    S_START,   // «можно начинать» - менеджер открывает, когда все подключились
    S_ROBOT,   // «у роботов могла появиться работа»
    S_KIT,     // «в комплектах что-то изменилось»
    S_RELEASE, // «складу нужно выпустить замену списанной детали»
    S_FIRST_STORE
};

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
    OP_HELLO,     // подключиться: who = роль*1000 + номер
    OP_BYE,       // отключиться
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
    int sem;              // семафоры: sem - свободные места, sem+1 - готовые детали
} Store;

typedef struct Actor
{ // Общее у всех участников
    char name[32];
    int joined;   // 1 - процесс подключился
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
    END_SIGNAL,
    END_SOFT
};

typedef struct Line
{
    Config cfg;             // копия параметров (рабочим процессам не нужен файл)
    int tick_ms;            // мс в единице модельного времени
    int quiet;              // тихий режим
    char log_path[256];     // журнал (пусто - без журнала)
    long start_clock;       // момент начала моделирования (CLOCK_MONOTONIC, мс)
    long last_event_ms;     // когда было последнее событие (для сторожа)
    long events;            // сколько событий выведено
    int n_sems;             // сколько семафоров используется
    int sem_init[MAX_SEMS]; // их начальные значения

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

    int raw_left[MAX_TYPES], required[MAX_TYPES];
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
    int soft_stop; // 1 - склад больше не выпускает заготовки (SIGUSR2)
    int end;       // причина завершения
    int signal_no; // сигнал, остановивший программу
    char why[256];
} Line;

extern Line *L; // линия (в разделяемой памяти или у менеджера)

void line_init(Line *l, const Config *c, int tick_ms, int quiet, const char *log_path);
int line_op(int op, int who, int out[4]);   // выполнить операцию (под замком!)
int line_expected(int role);                // сколько процессов этой роли
void line_start(void);                      // все подключились - начинаем
void stop_all(int reason, const char *why); // остановить всех (под замком)
void diagnose(void);                        // почему линия встала (под замком)
void watchdog(void);                        // проверки сторожа (под замком)
void check_final(void);                     // итоговая проверка правил
void print_status(void);                    // текущее состояние (SIGUSR1)
void report(void);                          // итоги
int exit_code(void);                        // код завершения по причине
const char *role_name(int role);

#endif
