#include <iostream>
#include <vector>
#include <queue>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <map>

#include "message.h"

std::queue<int> clientQueue;
std::vector<int> clients;
std::map<int,std::string> nicknames;

pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

bool sendMessage(int sock, uint8_t type, const std::string &text)
{
    Message msg{};

    msg.type = type;

    strncpy(msg.payload, text.c_str(), MAX_PAYLOAD);

    msg.length = htonl(sizeof(msg.type) + strlen(msg.payload));

    return send(sock, &msg, sizeof(msg), 0) > 0;
}

bool recvMessage(int sock, Message &msg)
{
    int bytes = recv(sock, &msg, sizeof(msg), 0);

    if (bytes <= 0)
        return false;

    msg.length = ntohl(msg.length);

    return true;
}

void broadcast(const std::string &text, int sender)
{
    pthread_mutex_lock(&clientsMutex);

    for (int client : clients)
    {
        if (client != sender)
            sendMessage(client, MSG_TEXT, text);
    }

    pthread_mutex_unlock(&clientsMutex);
}

void removeClient(int sock)
{
    pthread_mutex_lock(&clientsMutex);

    for (auto it = clients.begin(); it != clients.end(); ++it)
    {
        if (*it == sock)
        {
            clients.erase(it);
            break;
        }
    }

    pthread_mutex_unlock(&clientsMutex);
}

void *worker(void *arg)
{
    while (true)
    {
        pthread_mutex_lock(&queueMutex);

        while (clientQueue.empty())
            pthread_cond_wait(&queueCond, &queueMutex);

        int client = clientQueue.front();

        clientQueue.pop();

        pthread_mutex_unlock(&queueMutex);

        Message msg{};

        if (!recvMessage(client, msg))
        {
            close(client);
            continue;
        }

        if (msg.type != MSG_HELLO)
        {
            close(client);
            continue;
        }

        std::string nickname = msg.payload;

        pthread_mutex_lock(&clientsMutex);

        clients.push_back(client);
        nicknames[client] = nickname;

        int online = clients.size();

        pthread_mutex_unlock(&clientsMutex);

        std::string welcome =
        "Welcome " + nickname +
        " | Online: " +
        std::to_string(online);

        sendMessage(client, MSG_WELCOME, welcome);

        std::cout << nickname << " connected" << std::endl;
        std::cout << "Clients online: " << online << std::endl;

        broadcast(nickname + " joined the chat", client);

        while (true)
        {
            if (!recvMessage(client, msg))
                break;

            if (msg.type == MSG_TEXT)
            {
                std::string text = msg.payload;

                std::cout << text << std::endl;

                broadcast(text, client);
            }

            if (msg.type == MSG_PING)
                sendMessage(client, MSG_PONG, "");

            if (msg.type == MSG_BYE)
                break;
        }

        std::string name = nicknames[client];

        std::cout << name << " disconnected" << std::endl;

        removeClient(client);

        pthread_mutex_lock(&clientsMutex);
        nicknames.erase(client);
        online = clients.size();
        pthread_mutex_unlock(&clientsMutex);

        std::cout << "Clients online: " << online << std::endl;

        broadcast(name + " left the chat", client);

        close(client);
    }

    return NULL;
}

int main()
{
    int server_fd;

    sockaddr_in address{};

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    address.sin_family = AF_INET;

    address.sin_addr.s_addr = INADDR_ANY;

    address.sin_port = htons(PORT);

    bind(server_fd, (sockaddr *)&address, sizeof(address));

    listen(server_fd, 10);

    pthread_t threads[THREAD_POOL_SIZE];

    for (int i = 0; i < THREAD_POOL_SIZE; i++)
        pthread_create(&threads[i], NULL, worker, NULL);

    std::cout << "Server started" << std::endl;

    while (true)
    {
        int client;

        sockaddr_in client_addr;

        socklen_t len = sizeof(client_addr);

        client = accept(server_fd,
                        (sockaddr *)&client_addr,
                        &len);

        pthread_mutex_lock(&queueMutex);

        clientQueue.push(client);

        pthread_cond_signal(&queueCond);

        pthread_mutex_unlock(&queueMutex);
    }

    close(server_fd);

    return 0;
}
