#ifndef LOG_H
#define LOG_H
//-------------------------------------------------------------------------
//  вывод событий на экран и в журнал
//-------------------------------------------------------------------------

long clock_ms(void); // монотонные часы процесса, мс

// Сервер решает, кому отправлять строку, и пишет её в журнал.
void log_init(int tick_ms, long *start, long *events, void (*output)(const char *, int));
long now_ms(void); // миллисекунды с начала моделирования
long now_t(void);  // модельное время, единицы

// Строка события: время, категория, участник, описание
void event(const char *cat, const char *who, const char *fmt, ...);

void say(const char *fmt, ...);

#endif
