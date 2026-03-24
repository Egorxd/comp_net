#include <iostream>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

#include "message.h"

int sock;

bool sendMessage(uint8_t type, const std::string &text)
{
    Message msg{};

    msg.type = type;

    strncpy(msg.payload, text.c_str(), MAX_PAYLOAD);

    msg.length = htonl(sizeof(msg.type) + strlen(msg.payload));

    return send(sock, &msg, sizeof(msg), 0) > 0;
}

bool recvMessage(Message &msg)
{
    int bytes = recv(sock, &msg, sizeof(msg), 0);

    if (bytes <= 0)
        return false;

    msg.length = ntohl(msg.length);

    return true;
}

bool connectServer(const std::string& nickname)
{
    sockaddr_in addr{};

    sock = socket(AF_INET, SOCK_STREAM, 0);

    addr.sin_family = AF_INET;

    addr.sin_port = htons(PORT);

    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock,
                (sockaddr *)&addr,
                sizeof(addr)) < 0)
        return false;

    sendMessage(MSG_HELLO, nickname);

    Message msg{};

    if (!recvMessage(msg))
        return false;

    if(msg.type == MSG_WELCOME)
        std::cout << msg.payload << std::endl;

    std::cout << "Connected" << std::endl;

    return true;
}

void receiver()
{
    Message msg{};

    while (true)
    {
        if (!recvMessage(msg))
        {
            std::cout << "Disconnected" << std::endl;

            close(sock);

            break;
        }

        if (msg.type == MSG_TEXT)
            std::cout << msg.payload << std::endl;

        if (msg.type == MSG_PONG)
            std::cout << "PONG" << std::endl;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout << "Usage: ./client nickname" << std::endl;
        return 1;
    }

    std::string nickname = argv[1];

    while (!connectServer(nickname))
    {
        std::cout << "Reconnect..." << std::endl;

        sleep(2);
    }

    std::thread t(receiver);

    std::string input;

    while (true)
    {
        std::getline(std::cin, input);

        if (input == "/ping")
            sendMessage(MSG_PING, "");

        else if (input == "/quit")
        {
            sendMessage(MSG_BYE, "");

            close(sock);

            break;
        }

        else
        {
            std::string msg = nickname + ": " + input;

            sendMessage(MSG_TEXT, msg);
        }
    }

    t.join();

    return 0;
}
