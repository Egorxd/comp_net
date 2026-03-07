#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "message.h"

#define PORT 5000

bool sendMessage(int sock, uint8_t type, const std::string& text)
{
    Message msg{};
    msg.type = type;

    if (!text.empty())
        strncpy(msg.payload, text.c_str(), MAX_PAYLOAD - 1);

    msg.length = htonl(sizeof(msg.type) + strlen(msg.payload));

    return send(sock, &msg, sizeof(Message), 0) > 0;
}

bool recvMessage(int sock, Message& msg)
{
    ssize_t received = recv(sock, &msg, sizeof(Message), 0);
    if (received <= 0)
        return false;

    msg.length = ntohl(msg.length);
    return true;
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0)
    {
        perror("bind");
        return 1;
    }

    listen(server_fd, 1);
    std::cout << "Server started on port " << PORT << std::endl;

    sockaddr_in client_addr{};
    socklen_t addrlen = sizeof(client_addr);

    int client_socket = accept(server_fd, (sockaddr*)&client_addr, &addrlen);
    if (client_socket < 0)
    {
        perror("accept");
        return 1;
    }

    std::string client_ip = inet_ntoa(client_addr.sin_addr);
    int client_port = ntohs(client_addr.sin_port);

    std::cout << "Client connected" << std::endl;

    Message msg{};
    if (!recvMessage(client_socket, msg) || msg.type != MSG_HELLO)
    {
        close(client_socket);
        close(server_fd);
        return 1;
    }

    std::cout << "[" << client_ip << ":" << client_port << "]: "
              << msg.payload << std::endl;

    sendMessage(client_socket, MSG_WELCOME,
                client_ip + ":" + std::to_string(client_port));

    while (true)
    {
        if (!recvMessage(client_socket, msg))
        {
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        switch (msg.type)
        {
            case MSG_TEXT:
                std::cout << "[" << client_ip << ":" << client_port
                          << "]: " << msg.payload << std::endl;
                break;

            case MSG_PING:
                sendMessage(client_socket, MSG_PONG, "");
                break;

            case MSG_BYE:
                std::cout << "Client disconnected" << std::endl;
                sendMessage(client_socket, MSG_BYE, "");
                close(client_socket);
                close(server_fd);
                return 0;
        }
    }

    close(client_socket);
    close(server_fd);
    return 0;
}