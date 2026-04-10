#include <iostream>
#include <thread>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

#include "message.h"

int sock = -1;
bool running = true;

bool sendAll(int sockFd, const void *data, size_t size)
{
    const char *buffer = static_cast<const char *>(data);
    size_t sent = 0;

    while (sent < size)
    {
        ssize_t result = send(sockFd, buffer + sent, size - sent, 0);
        if (result <= 0)
            return false;
        sent += static_cast<size_t>(result);
    }

    return true;
}

bool recvAll(int sockFd, void *data, size_t size)
{
    char *buffer = static_cast<char *>(data);
    size_t received = 0;

    while (received < size)
    {
        ssize_t result = recv(sockFd, buffer + received, size - received, 0);
        if (result <= 0)
            return false;
        received += static_cast<size_t>(result);
    }

    return true;
}

bool sendMessage(uint8_t type, const std::string &text)
{
    Message msg{};

    msg.type = type;
    std::strncpy(msg.payload, text.c_str(), MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = htonl(static_cast<uint32_t>(sizeof(msg.type) + std::strlen(msg.payload)));

    return sendAll(sock, &msg, sizeof(msg));
}

bool recvMessage(Message &msg)
{
    if (!recvAll(sock, &msg, sizeof(msg)))
        return false;

    msg.length = ntohl(msg.length);
    return true;
}

bool connectServer(const std::string &nickname)
{
    sockaddr_in addr{};
    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
        return false;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        sock = -1;
        return false;
    }

    if (!sendMessage(MSG_HELLO, nickname))
        return false;

    Message msg{};
    if (!recvMessage(msg) || msg.type != MSG_WELCOME)
        return false;

    std::cout << msg.payload << std::endl;

    if (!sendMessage(MSG_AUTH, nickname))
        return false;

    if (!recvMessage(msg))
        return false;

    if (msg.type == MSG_ERROR)
    {
        std::cout << msg.payload << std::endl;
        return false;
    }

    if (msg.type == MSG_SERVER_INFO)
        std::cout << msg.payload << std::endl;

    std::cout << "Connected" << std::endl;
    return true;
}

void receiver()
{
    Message msg{};

    while (running)
    {
        if (!recvMessage(msg))
        {
            if (running)
                std::cout << "Disconnected" << std::endl;
            running = false;
            if (sock >= 0)
                close(sock);
            break;
        }

        if (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE || msg.type == MSG_SERVER_INFO || msg.type == MSG_ERROR)
            std::cout << msg.payload << std::endl;
        else if (msg.type == MSG_PONG)
            std::cout << "PONG" << std::endl;
    }
}

bool parsePrivateCommand(const std::string &input, std::string &target, std::string &text)
{
    if (input.rfind("/w ", 0) != 0)
        return false;

    std::string rest = input.substr(3);
    if (rest.empty())
        return false;

    if (rest[0] == '"')
    {
        size_t closingQuote = rest.find('"', 1);
        if (closingQuote == std::string::npos)
            return false;

        target = rest.substr(1, closingQuote - 1);

        if (closingQuote + 1 >= rest.size() || rest[closingQuote + 1] != ' ')
            return false;

        text = rest.substr(closingQuote + 2);
    }
    else
    {
        size_t firstSpace = rest.find(' ');
        if (firstSpace == std::string::npos)
            return false;

        target = rest.substr(0, firstSpace);
        text = rest.substr(firstSpace + 1);
    }

    return !target.empty() && !text.empty();
}

int main()
{
    std::string nickname;
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nickname);

    while (nickname.empty())
    {
        std::cout << "Nickname must not be empty. Enter nickname: ";
        std::getline(std::cin, nickname);
    }

    while (!connectServer(nickname))
    {
        std::cout << "Reconnect..." << std::endl;
        sleep(2);
    }

    std::thread t(receiver);
    std::string input;

    while (running)
    {
        if (!std::getline(std::cin, input))
            break;

        if (input == "/ping")
        {
            if (!sendMessage(MSG_PING, ""))
                break;
        }
        else if (input == "/quit")
        {
            sendMessage(MSG_BYE, "");
            running = false;
            if (sock >= 0)
                close(sock);
            break;
        }
        else if (input.rfind("/w ", 0) == 0)
        {
            std::string target;
            std::string text;

            if (!parsePrivateCommand(input, target, text))
            {
                std::cout << "Usage: /w <nick> <message> or /w \"nick with spaces\" <message>" << std::endl;
                continue;
            }

            if (!sendMessage(MSG_PRIVATE, target + ":" + text))
                break;
        }
        else
        {
            if (!sendMessage(MSG_TEXT, input))
                break;
        }
    }

    running = false;
    if (sock >= 0)
        shutdown(sock, SHUT_RDWR);

    t.join();
    return 0;
}
