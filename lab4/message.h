#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>

#define MAX_PAYLOAD 1024
#define MAX_NICKNAME 32
#define PORT 9000
#define THREAD_POOL_SIZE 10

typedef struct
{
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
} Message;

typedef struct
{
    int sock;
    char nickname[MAX_NICKNAME];
    int authenticated;
} Client;

enum
{
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10
};

#endif
