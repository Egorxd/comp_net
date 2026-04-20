#include <arpa/inet.h>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "message.h"

int sock = -1;
bool running = true;
std::string currentNickname;
uint32_t nextLocalMessageId = 1;

std::string trim(const std::string &value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";

    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

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

MessageEx makeMessage(uint8_t type,
                      const std::string &sender,
                      const std::string &receiver,
                      const std::string &payload)
{
    MessageEx msg{};
    msg.type = type;
    msg.msg_id = nextLocalMessageId++;
    msg.timestamp = std::time(nullptr);

    std::strncpy(msg.sender, sender.c_str(), MAX_NAME - 1);
    std::strncpy(msg.receiver, receiver.c_str(), MAX_NAME - 1);
    std::strncpy(msg.payload, payload.c_str(), MAX_PAYLOAD - 1);

    uint32_t usefulLength = static_cast<uint32_t>(sizeof(msg.type) + sizeof(msg.msg_id) + sizeof(msg.timestamp) +
                                                  std::strlen(msg.sender) + std::strlen(msg.receiver) + std::strlen(msg.payload));
    msg.length = htonl(usefulLength);
    return msg;
}

bool sendMessageEx(uint8_t type, const std::string &receiver, const std::string &payload, const std::string &sender = "")
{
    MessageEx msg = makeMessage(type, sender, receiver, payload);
    return sendAll(sock, &msg, sizeof(msg));
}

bool recvMessageEx(MessageEx &msg)
{
    if (!recvAll(sock, &msg, sizeof(msg)))
        return false;

    msg.length = ntohl(msg.length);
    return true;
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
        size_t spacePos = rest.find(' ');
        if (spacePos == std::string::npos)
            return false;

        target = rest.substr(0, spacePos);
        text = rest.substr(spacePos + 1);
    }

    target = trim(target);
    text = trim(text);
    return !target.empty() && !text.empty();
}

void printHelp()
{
    std::cout << "Available commands:\n";
    std::cout << "/help\n";
    std::cout << "/list\n";
    std::cout << "/history\n";
    std::cout << "/history N\n";
    std::cout << "/quit\n";
    std::cout << "/w <nick> <message>\n";
    std::cout << "/ping\n";
    std::cout << "Tip: packets never sleep" << std::endl;
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

    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        close(sock);
        sock = -1;
        return false;
    }

    if (!sendMessageEx(MSG_HELLO, "", "HELLO"))
        return false;

    MessageEx msg{};
    if (!recvMessageEx(msg) || msg.type != MSG_WELCOME)
        return false;

    std::cout << msg.payload << std::endl;

    if (!sendMessageEx(MSG_AUTH, "", nickname, nickname))
        return false;

    if (!recvMessageEx(msg))
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
    MessageEx msg{};

    while (running)
    {
        if (!recvMessageEx(msg))
        {
            if (running)
                std::cout << "Disconnected" << std::endl;
            running = false;
            if (sock >= 0)
                close(sock);
            break;
        }

        if (std::strlen(msg.payload) > 0)
            std::cout << msg.payload << std::endl;
    }
}

int main()
{
    std::cout << "Enter nickname: ";
    std::getline(std::cin, currentNickname);
    currentNickname = trim(currentNickname);

    while (currentNickname.empty())
    {
        std::cout << "Nickname must not be empty. Enter nickname: ";
        std::getline(std::cin, currentNickname);
        currentNickname = trim(currentNickname);
    }

    while (!connectServer(currentNickname))
    {
        std::cout << "Reconnect..." << std::endl;
        sleep(2);
    }

    std::thread receiverThread(receiver);
    std::string input;

    while (running)
    {
        if (!std::getline(std::cin, input))
            break;

        input = trim(input);
        if (input.empty())
            continue;

        if (input == "/help")
        {
            printHelp();
        }
        else if (input == "/list")
        {
            if (!sendMessageEx(MSG_LIST, "", "", currentNickname))
                break;
        }
        else if (input == "/history")
        {
            if (!sendMessageEx(MSG_HISTORY, "", "", currentNickname))
                break;
        }
        else if (input.rfind("/history ", 0) == 0)
        {
            std::string amount = trim(input.substr(9));
            if (amount.empty())
            {
                std::cout << "Usage: /history N" << std::endl;
                continue;
            }

            bool valid = true;
            for (char ch : amount)
            {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                {
                    valid = false;
                    break;
                }
            }

            if (!valid)
            {
                std::cout << "Usage: /history N" << std::endl;
                continue;
            }

            if (!sendMessageEx(MSG_HISTORY, "", amount, currentNickname))
                break;
        }
        else if (input == "/ping")
        {
            if (!sendMessageEx(MSG_PING, "", "", currentNickname))
                break;
        }
        else if (input == "/quit")
        {
            sendMessageEx(MSG_BYE, "", "", currentNickname);
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

            if (!sendMessageEx(MSG_PRIVATE, target, target + ":" + text, currentNickname))
                break;
        }
        else
        {
            if (!sendMessageEx(MSG_TEXT, "", input, currentNickname))
                break;
        }
    }

    running = false;
    if (sock >= 0)
        shutdown(sock, SHUT_RDWR);

    receiverThread.join();
    return 0;
}
