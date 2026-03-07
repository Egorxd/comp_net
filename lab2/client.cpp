#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
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
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        return 1;
    }

    std::cout << "Connected" << std::endl;

    sendMessage(sock, MSG_HELLO, "Client");

    Message msg{};
    if (!recvMessage(sock, msg) || msg.type != MSG_WELCOME)
    {
        std::cout << "No welcome from server\n";
        return 1;
    }

    std::cout << "Welcome " << msg.payload << std::endl;

    std::thread receiver([&]()
    {
        while (true)
        {
            if (!recvMessage(sock, msg))
            {
                std::cout << "Disconnected" << std::endl;
                exit(0);
            }

            if (msg.type == MSG_PONG)
                std::cout << "PONG" << std::endl;

            else if (msg.type == MSG_TEXT)
                std::cout << msg.payload << std::endl;

            else if (msg.type == MSG_BYE)
            {
                std::cout << "Disconnected" << std::endl;
                exit(0);
            }
        }
    });

    while (true)
    {
        std::string input;
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "/ping")
            sendMessage(sock, MSG_PING, "");

        else if (input == "/quit")
        {
            sendMessage(sock, MSG_BYE, "");
            break;
        }
        else
            sendMessage(sock, MSG_TEXT, input);
    }

    receiver.join();
    close(sock);
    return 0;
}