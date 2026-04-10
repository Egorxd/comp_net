#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "message.h"

std::queue<int> clientQueue;
std::vector<Client> clients;

pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

std::string typeToString(uint8_t type)
{
    switch (type)
    {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        default: return "UNKNOWN";
    }
}

void logIncoming(const Message &msg)
{
    std::cout << "[Layer 4 - Transport] recv()" << std::endl;
    std::cout << "[Layer 6 - Presentation] deserialize " << typeToString(msg.type) << std::endl;
}

void logOutgoing(uint8_t type, const std::string &appInfo)
{
    std::cout << "[Layer 7 - Application] prepare response: " << appInfo << std::endl;
    std::cout << "[Layer 6 - Presentation] serialize " << typeToString(type) << std::endl;
    std::cout << "[Layer 4 - Transport] send()" << std::endl;
}

bool sendAll(int sock, const void *data, size_t size)
{
    const char *buffer = static_cast<const char *>(data);
    size_t sent = 0;

    while (sent < size)
    {
        ssize_t result = send(sock, buffer + sent, size - sent, 0);
        if (result <= 0)
            return false;
        sent += static_cast<size_t>(result);
    }

    return true;
}

bool recvAll(int sock, void *data, size_t size)
{
    char *buffer = static_cast<char *>(data);
    size_t received = 0;

    while (received < size)
    {
        ssize_t result = recv(sock, buffer + received, size - received, 0);
        if (result <= 0)
            return false;
        received += static_cast<size_t>(result);
    }

    return true;
}

bool sendMessage(int sock, uint8_t type, const std::string &text, const std::string &appInfo)
{
    Message msg{};

    msg.type = type;
    std::strncpy(msg.payload, text.c_str(), MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = htonl(static_cast<uint32_t>(sizeof(msg.type) + std::strlen(msg.payload)));

    logOutgoing(type, appInfo);
    return sendAll(sock, &msg, sizeof(msg));
}

bool recvMessage(int sock, Message &msg)
{
    if (!recvAll(sock, &msg, sizeof(msg)))
        return false;

    msg.length = ntohl(msg.length);
    logIncoming(msg);
    return true;
}

std::string getNickname(const Client &client)
{
    return std::string(client.nickname);
}

bool parsePrivatePayload(const std::string &payload, std::string &target, std::string &text)
{
    size_t pos = payload.find(':');
    if (pos == std::string::npos)
        return false;

    target = payload.substr(0, pos);
    text = payload.substr(pos + 1);

    return !target.empty() && !text.empty();
}

void broadcastMessage(uint8_t type, const std::string &text, int excludeSock, const std::string &appInfo)
{
    pthread_mutex_lock(&clientsMutex);

    for (const Client &client : clients)
    {
        if (client.authenticated && client.sock != excludeSock)
            sendMessage(client.sock, type, text, appInfo);
    }

    pthread_mutex_unlock(&clientsMutex);
}

bool sendPrivateMessage(const std::string &targetNick, const std::string &text, const std::string &appInfo)
{
    pthread_mutex_lock(&clientsMutex);

    for (const Client &client : clients)
    {
        if (client.authenticated && targetNick == client.nickname)
        {
            bool ok = sendMessage(client.sock, MSG_PRIVATE, text, appInfo);
            pthread_mutex_unlock(&clientsMutex);
            return ok;
        }
    }

    pthread_mutex_unlock(&clientsMutex);
    return false;
}

bool addAuthenticatedClient(int sock, const std::string &nickname)
{
    pthread_mutex_lock(&clientsMutex);

    for (const Client &client : clients)
    {
        if (client.authenticated && nickname == client.nickname)
        {
            pthread_mutex_unlock(&clientsMutex);
            return false;
        }
    }

    Client client{};
    client.sock = sock;
    client.authenticated = 1;
    std::strncpy(client.nickname, nickname.c_str(), MAX_NICKNAME - 1);
    client.nickname[MAX_NICKNAME - 1] = '\0';
    clients.push_back(client);

    pthread_mutex_unlock(&clientsMutex);
    return true;
}

bool removeClient(int sock, Client &removed)
{
    pthread_mutex_lock(&clientsMutex);

    for (auto it = clients.begin(); it != clients.end(); ++it)
    {
        if (it->sock == sock)
        {
            removed = *it;
            clients.erase(it);
            pthread_mutex_unlock(&clientsMutex);
            return true;
        }
    }

    pthread_mutex_unlock(&clientsMutex);
    return false;
}

void *worker(void *arg)
{
    (void)arg;

    while (true)
    {
        pthread_mutex_lock(&queueMutex);

        while (clientQueue.empty())
            pthread_cond_wait(&queueCond, &queueMutex);

        int clientSock = clientQueue.front();
        clientQueue.pop();

        pthread_mutex_unlock(&queueMutex);

        Message msg{};
        bool authenticated = false;
        std::string nickname;

        if (!recvMessage(clientSock, msg))
        {
            close(clientSock);
            continue;
        }

        std::cout << "[Layer 7 - Application] handle " << typeToString(msg.type) << std::endl;

        if (msg.type != MSG_HELLO)
        {
            sendMessage(clientSock, MSG_ERROR,
                        "[SERVER][ERROR]: HELLO expected",
                        "reject connection without HELLO");
            close(clientSock);
            continue;
        }

        sendMessage(clientSock, MSG_WELCOME,
                    "[SERVER]: Service handshake completed",
                    "send welcome after HELLO");

        while (!authenticated)
        {
            if (!recvMessage(clientSock, msg))
                break;

            std::cout << "[Layer 5 - Session] client not authenticated" << std::endl;
            std::cout << "[Layer 7 - Application] handle " << typeToString(msg.type) << std::endl;

            if (msg.type != MSG_AUTH)
                continue;

            nickname = msg.payload;

            if (nickname.empty())
            {
                std::cout << "[Layer 5 - Session] authentication failed: empty nickname" << std::endl;
                sendMessage(clientSock, MSG_ERROR,
                            "[SERVER][ERROR]: nickname must not be empty",
                            "authentication error: empty nickname");
                close(clientSock);
                clientSock = -1;
                break;
            }

            if (!addAuthenticatedClient(clientSock, nickname))
            {
                std::cout << "[Layer 5 - Session] authentication failed: duplicate nickname" << std::endl;
                sendMessage(clientSock, MSG_ERROR,
                            "[SERVER][ERROR]: nickname already in use",
                            "authentication error: duplicate nickname");
                close(clientSock);
                clientSock = -1;
                break;
            }

            authenticated = true;
            std::cout << "[Layer 5 - Session] authentication success" << std::endl;
            sendMessage(clientSock, MSG_SERVER_INFO,
                        "[SERVER]: authenticated as [" + nickname + "]",
                        "authentication success info");

            std::cout << "User [" << nickname << "] connected" << std::endl;
            broadcastMessage(MSG_SERVER_INFO,
                             "[SERVER]: User [" + nickname + "] connected",
                             clientSock,
                             "broadcast user connected");
        }

        if (clientSock == -1)
            continue;

        if (!authenticated)
        {
            close(clientSock);
            continue;
        }

        while (true)
        {
            if (!recvMessage(clientSock, msg))
                break;

            std::cout << "[Layer 5 - Session] client authenticated as [" << nickname << "]" << std::endl;
            std::cout << "[Layer 7 - Application] handle " << typeToString(msg.type) << std::endl;

            if (msg.type == MSG_TEXT)
            {
                std::string text = "[" + nickname + "]: " + std::string(msg.payload);
                std::cout << text << std::endl;
                broadcastMessage(MSG_TEXT, text, clientSock, "broadcast text message");
            }
            else if (msg.type == MSG_PRIVATE)
            {
                std::string target;
                std::string privateText;

                if (!parsePrivatePayload(msg.payload, target, privateText))
                {
                    sendMessage(clientSock, MSG_ERROR,
                                "[SERVER][ERROR]: private message format must be target:message",
                                "private message parsing error");
                    continue;
                }

                std::string formatted = "[PRIVATE][" + nickname + "]: " + privateText;

                if (!sendPrivateMessage(target, formatted, "route private message"))
                {
                    sendMessage(clientSock, MSG_ERROR,
                                "[SERVER][ERROR]: user [" + target + "] not found",
                                "private message target not found");
                }
            }
            else if (msg.type == MSG_PING)
            {
                sendMessage(clientSock, MSG_PONG,
                            "PONG",
                            "reply to ping");
            }
            else if (msg.type == MSG_BYE)
            {
                break;
            }
            else
            {
                sendMessage(clientSock, MSG_ERROR,
                            "[SERVER][ERROR]: unsupported message type",
                            "unsupported message type");
            }
        }

        Client removed{};
        if (removeClient(clientSock, removed) && removed.authenticated)
        {
            std::string removedNick = getNickname(removed);
            std::cout << "User [" << removedNick << "] disconnected" << std::endl;
            broadcastMessage(MSG_SERVER_INFO,
                             "[SERVER]: User [" + removedNick + "] disconnected",
                             clientSock,
                             "broadcast user disconnected");
        }

        close(clientSock);
    }

    return NULL;
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        std::cerr << "socket() failed" << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr *)&address, sizeof(address)) < 0)
    {
        std::cerr << "bind() failed" << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        std::cerr << "listen() failed" << std::endl;
        close(server_fd);
        return 1;
    }

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i)
        pthread_create(&threads[i], NULL, worker, NULL);

    std::cout << "Server started on port " << PORT << std::endl;

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int clientSock = accept(server_fd, (sockaddr *)&client_addr, &len);

        if (clientSock < 0)
            continue;

        std::cout << "Client connected" << std::endl;

        pthread_mutex_lock(&queueMutex);
        clientQueue.push(clientSock);
        pthread_cond_signal(&queueCond);
        pthread_mutex_unlock(&queueMutex);
    }

    close(server_fd);
    return 0;
}
