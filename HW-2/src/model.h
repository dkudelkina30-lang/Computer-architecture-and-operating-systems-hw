#ifndef MODEL_H
#define MODEL_H

//-------------------------------------------------------------------------
//  участники модели сборочной линии
//-------------------------------------------------------------------------
//  Каждый участник с поведением - отдельный поток:
//   склад (1 поток), станки, контролёры ОТК, роботы, сборочные посты.
//  Общие данные линии защищены одним мьютексом line.lock.
//  Ожидание (места в накопителе, детали, работы) через счётчики Counter
//-------------------------------------------------------------------------

#include <pthread.h>
#include "config.h"
#include "counter.h"

#define MAX_PARTS 1024 // максимум деталей за один прогон

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
    int type;    // тип детали (индекс в Config.pt)
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
    int count;            // сколько лежит
    int cap;              // вместимость
    int max;              // наибольшее заполнение (статистика)
    Counter free;         // свободные места (робот бронирует место заранее)
    Counter ready;        // детали, которые можно взять (очереди станков, вход ОТК)
} Store;

typedef struct Machine
{ // Станок
    pthread_t tid;
    char name[32];
    int id, type; // номер станка и тип станка
    int part;     // деталь на станке
    int ops;      // выполнено операций
    long busy_ms, blocked_ms;
    Store out; // выходной накопитель
} Machine;

typedef struct Inspector
{ // Контролёр ОТК
    pthread_t tid;
    char name[32];
    int id, part;
    int checked, defects;
    long busy_ms, blocked_ms;
    unsigned seed; // своё состояние генератора для rand_r
} Inspector;

typedef struct Robot
{ // Транспортный робот
    pthread_t tid;
    char name[32];
    int id, part;
    int trips;
    long busy_ms;
} Robot;

typedef struct Station
{ // Сборочный пост
    pthread_t tid;
    char name[32];
    int id;
    int parts[MAX_KIT], n; // детали собираемого комплекта
    int built;
    long kit_born; // когда создана самая ранняя деталь комплекта
    long busy_ms;
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
{ // Сборочная линия - общие данные всех потоков
    const Config *cfg;
    pthread_mutex_t lock; // защищает все поля ниже

    Part parts[MAX_PARTS];
    int n_parts;
    Store warehouse, qc_in, qc_out;
    Store queue[MAX_TYPES]; // очереди к станкам по типам станков
    Store kit[MAX_TYPES];   // комплектующие по типам деталей
    Machine mach[MAX_MACHINES];
    int n_mach;
    Inspector insp[MAX_ACTORS];
    Robot rob[MAX_ACTORS];
    Station st[MAX_ACTORS];

    Counter robot_work;   // «для роботов что-то изменилось»
    Counter kit_event;    // «в комплектах что-то изменилось»
    Counter release_wake; // «складу нужно выпустить замену»

    int raw_left[MAX_TYPES], required[MAX_TYPES]; // осталось заготовок/нужно по плану
    int on_line[MAX_TYPES], consumed[MAX_TYPES];  // в работе/вошло в изделия
    int created[MAX_TYPES], scrapped[MAX_TYPES], defects[MAX_TYPES], reworks[MAX_TYPES];
    int produced[MAX_PRODUCTS], in_asm[MAX_PRODUCTS], product_no;
    int release_rr; // чья очередь выпускать заготовку
    long cycle_sum; // сумма циклов изделий (от первой детали до выпуска)
    long lead_sum[MAX_TYPES];
    int lead_n[MAX_TYPES]; // путь детали от склада до комплекта
    long checks;           // сколько проверок правил выполнено

    int stop;      // 1 - всем потокам пора завершаться
    int soft_stop; // 1 - склад больше не выпускает заготовки (SIGUSR2)
    int end;       // причина завершения
    int signal_no; // номер сигнала, остановившего программу
    char why[256];
    int violations; // найдено нарушений правил
} Line;

extern Line line;

// потоки участников
void *warehouse_thread(void *arg);
void *machine_thread(void *arg);
void *inspector_thread(void *arg);
void *robot_thread(void *arg);
void *station_thread(void *arg);
void line_init(const Config *c, int tick_ms); // tick_ms - мс в единице модельного времени
void line_destroy(void);
int stopping(void);                         // пора ли завершаться
void stop_all(int reason, const char *why); // остановить всех
void diagnose(void);                        // почему линия встала
int check_final(void);                      // итоговая проверка правил, возвращает число нарушений
void print_status(void);                    // текущее состояние линии

#endif
