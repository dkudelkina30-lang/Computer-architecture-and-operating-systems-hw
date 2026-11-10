#ifndef MQ_H
#define MQ_H
//-------------------------------------------------------------------------
//  сообщения между рабочими процессами и менеджером
//-------------------------------------------------------------------------

typedef struct Request
{             // запрос менеджеру
    int op;   // операция (OP_...)
    int who;  // номер участника (для HELLO/BYE - роль*1000 + номер)
    int slot; // в какую очередь отвечать
} Request;

typedef struct Reply
{               // ответ менеджера
    int r;      // результат операции
    int out[4]; // дополнительные данные (номера семафоров и т. п.)
} Reply;

void mq_req_name(char *buf, const char *name);
void mq_rep_name(char *buf, const char *name, int slot);
int mq_worker_open(const char *name, int slot);
void mq_worker_close(void);

#endif
