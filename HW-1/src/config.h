#ifndef CONFIG_H
#define CONFIG_H
//--------------------------------------------
//  Входные параметры модели сборочной линии
//--------------------------------------------

#define MAX_NAME 24     // Длина имени станка, детали, изделия
#define MAX_TYPES 16    // Типов станков и типов деталей
#define MAX_PRODUCTS 8  // Видов изделий
#define MAX_ROUTE 12    // Операций в маршруте детали
#define MAX_MACHINES 64 // Станков всего
#define MAX_ACTORS 32   // Контролёров, роботов, сборочных постов
#define MAX_KIT 64      // Деталей в одном комплекте

typedef struct Step
{                 // Операция маршрута
    int mtype;    // Тип станка
    int duration; // Длительность операции
} Step;

typedef struct MachineType
{ // Тип станка
    char name[MAX_NAME];
    int count; // Сколько станков этого типа
} MachineType;

typedef struct PartType
{ // Тип детали
    char name[MAX_NAME];
    Step route[MAX_ROUTE]; // Технологический маршрут
    int route_len;
    double defect;   // Вероятность брака при контроле
    int rework_step; // С какой операции (с 0) переделывать брак; -1 — списывать
    int max_rework;  // Сколько раз одну деталь можно переделать
    int raw;         // Запас заготовок
} PartType;

typedef struct Product
{ // Вид изделия
    char name[MAX_NAME];
    int need[MAX_TYPES]; // Сколько деталей каждого типа в комплекте
    int plan;            // План выпуска
} Product;

typedef struct Config
{
    MachineType mt[MAX_TYPES];
    int n_mt;
    PartType pt[MAX_TYPES];
    int n_pt;
    Product pr[MAX_PRODUCTS];
    int n_pr;
    int inspectors, inspect_time; // Контролёры ОТК и время контроля
    int stations, assembly_time;  // Сборочные посты и время сборки
    int robots, transport_time;   // Роботы и время перевозки
    int queue_cap;                // Вместимость входной очереди станков одного типа
    int outbuf_cap;               // Вместимость выходного накопителя станка
    int qc_in_cap;                // Вместимость входа ОТК
    int qc_out_cap;               // Вместимость выхода ОТК
    int kit_cap;                  // Вместимость накопителя комплектующих (на тип детали)
    int release_interval;         // Интервал запуска заготовок
    long seed;                    // Начальное значение ГСЧ (-1 - по времени)
    long max_time;                // Ограничение модельного времени
} Config;

// Загрузить конфигурацию из файла.
// 0 - успех, -1 - ошибка, текст ошибки с номером строки в err.
int config_load(Config *c, const char *path, char *err, int errlen);

// Проверить смысл данных: 0 - всё в порядке, 1 - ошибка в данных,
// 3 - данные корректны, но производство заведомо невозможно.
int config_check(const Config *c, char *err, int errlen);

// Вывести сводку параметров в файловый дескриптор
void config_print(const Config *c, int fd);

#endif
