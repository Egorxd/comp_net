#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>
#include <time.h>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32
#define PORT 9000
#define THREAD_POOL_SIZE 10
#define HISTORY_FILE "chat_history.json"
#define DEFAULT_HISTORY_COUNT 10

typedef struct {
    uint32_t length;
    uint8_t type;
    uint32_t msg_id;
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    time_t timestamp;
    char payload[MAX_PAYLOAD];
} MessageEx;

typedef struct {
    int sock;
    char nickname[MAX_NAME];
    int authenticated;
} Client;

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10,
    MSG_LIST = 11,
    MSG_HISTORY = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP = 14
};

#endif
