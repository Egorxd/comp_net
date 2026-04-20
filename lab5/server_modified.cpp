#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <queue>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <pthread.h>

#include "message.h"

struct HistoryRecord
{
    uint32_t msg_id;
    time_t timestamp;
    std::string sender;
    std::string receiver;
    uint8_t type;
    std::string text;
    bool delivered;
    bool is_offline;
};

struct OfflineMsg
{
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
};

std::queue<int> clientQueue;
std::vector<Client> clients;
std::vector<HistoryRecord> historyRecords;
std::vector<OfflineMsg> offlineQueue;
uint32_t nextMessageId = 1;

pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t historyMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t offlineMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

std::string trim(const std::string &value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";

    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string escapeJson(const std::string &value)
{
    std::string out;
    for (char ch : value)
    {
        switch (ch)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

std::string unescapeJson(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size())
        {
            char next = value[i + 1];
            switch (next)
            {
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += next; break;
            }
            ++i;
        }
        else
        {
            out += value[i];
        }
    }
    return out;
}

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
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP: return "MSG_HELP";
        default: return "UNKNOWN";
    }
}

uint8_t stringToType(const std::string &value)
{
    if (value == "MSG_HELLO") return MSG_HELLO;
    if (value == "MSG_WELCOME") return MSG_WELCOME;
    if (value == "MSG_TEXT") return MSG_TEXT;
    if (value == "MSG_PING") return MSG_PING;
    if (value == "MSG_PONG") return MSG_PONG;
    if (value == "MSG_BYE") return MSG_BYE;
    if (value == "MSG_AUTH") return MSG_AUTH;
    if (value == "MSG_PRIVATE") return MSG_PRIVATE;
    if (value == "MSG_ERROR") return MSG_ERROR;
    if (value == "MSG_SERVER_INFO") return MSG_SERVER_INFO;
    if (value == "MSG_LIST") return MSG_LIST;
    if (value == "MSG_HISTORY") return MSG_HISTORY;
    if (value == "MSG_HISTORY_DATA") return MSG_HISTORY_DATA;
    if (value == "MSG_HELP") return MSG_HELP;
    return MSG_SERVER_INFO;
}

std::string formatTime(time_t timestamp)
{
    char buffer[MAX_TIME_STR] = {0};
    std::tm tmValue{};
    localtime_r(&timestamp, &tmValue);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmValue);
    return buffer;
}

bool extractStringField(const std::string &object, const std::string &field, std::string &out)
{
    std::regex re("\\\"" + field + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
    std::smatch match;
    if (!std::regex_search(object, match, re))
        return false;

    out = unescapeJson(match[1].str());
    return true;
}

bool extractUnsignedField(const std::string &object, const std::string &field, uint32_t &out)
{
    std::regex re("\\\"" + field + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(object, match, re))
        return false;

    out = static_cast<uint32_t>(std::stoul(match[1].str()));
    return true;
}

bool extractTimeField(const std::string &object, const std::string &field, time_t &out)
{
    std::regex re("\\\"" + field + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(object, match, re))
        return false;

    out = static_cast<time_t>(std::stoll(match[1].str()));
    return true;
}

bool extractBoolField(const std::string &object, const std::string &field, bool &out)
{
    std::regex re("\\\"" + field + "\\\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(object, match, re))
        return false;

    out = match[1].str() == "true";
    return true;
}

void flushHistoryToFile()
{
    std::ofstream historyFile(HISTORY_FILE, std::ios::trunc);
    historyFile << "[\n";

    for (size_t i = 0; i < historyRecords.size(); ++i)
    {
        const HistoryRecord &record = historyRecords[i];
        historyFile << "  {\n";
        historyFile << "    \"msg_id\": " << record.msg_id << ",\n";
        historyFile << "    \"timestamp\": " << static_cast<long long>(record.timestamp) << ",\n";
        historyFile << "    \"sender\": \"" << escapeJson(record.sender) << "\",\n";
        historyFile << "    \"receiver\": \"" << escapeJson(record.receiver) << "\",\n";
        historyFile << "    \"type\": \"" << escapeJson(typeToString(record.type)) << "\",\n";
        historyFile << "    \"text\": \"" << escapeJson(record.text) << "\",\n";
        historyFile << "    \"delivered\": " << (record.delivered ? "true" : "false") << ",\n";
        historyFile << "    \"is_offline\": " << (record.is_offline ? "true" : "false") << "\n";
        historyFile << "  }";
        if (i + 1 != historyRecords.size())
            historyFile << ",";
        historyFile << "\n";
    }

    historyFile << "]\n";
}

void loadHistoryFromFile()
{
    std::ifstream historyFile(HISTORY_FILE);
    if (!historyFile)
        return;

    std::ostringstream ss;
    ss << historyFile.rdbuf();
    std::string content = ss.str();

    std::regex objectRegex("\\{[^\\}]*\\}");
    auto begin = std::sregex_iterator(content.begin(), content.end(), objectRegex);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        std::string object = it->str();
        HistoryRecord record{};
        std::string typeStr;

        if (!extractUnsignedField(object, "msg_id", record.msg_id))
            continue;
        if (!extractTimeField(object, "timestamp", record.timestamp))
            continue;
        if (!extractStringField(object, "sender", record.sender))
            continue;
        if (!extractStringField(object, "receiver", record.receiver))
            continue;
        if (!extractStringField(object, "type", typeStr))
            continue;
        if (!extractStringField(object, "text", record.text))
            continue;
        if (!extractBoolField(object, "delivered", record.delivered))
            continue;
        if (!extractBoolField(object, "is_offline", record.is_offline))
            continue;

        record.type = stringToType(typeStr);
        historyRecords.push_back(record);
        nextMessageId = std::max(nextMessageId, record.msg_id + 1);

        if (record.type == MSG_PRIVATE && record.is_offline && !record.delivered)
        {
            OfflineMsg offline{};
            std::strncpy(offline.sender, record.sender.c_str(), MAX_NAME - 1);
            std::strncpy(offline.receiver, record.receiver.c_str(), MAX_NAME - 1);
            std::strncpy(offline.text, record.text.c_str(), MAX_PAYLOAD - 1);
            offline.timestamp = record.timestamp;
            offline.msg_id = record.msg_id;
            offlineQueue.push_back(offline);
        }
    }
}

uint32_t getNextMessageId()
{
    pthread_mutex_lock(&historyMutex);
    uint32_t id = nextMessageId++;
    pthread_mutex_unlock(&historyMutex);
    return id;
}

void appendHistoryRecord(uint32_t msg_id,
                         time_t timestamp,
                         const std::string &sender,
                         const std::string &receiver,
                         uint8_t type,
                         const std::string &text,
                         bool delivered,
                         bool is_offline)
{
    pthread_mutex_lock(&historyMutex);
    historyRecords.push_back({msg_id, timestamp, sender, receiver, type, text, delivered, is_offline});
    flushHistoryToFile();
    pthread_mutex_unlock(&historyMutex);
}

void markHistoryDelivered(uint32_t msg_id)
{
    pthread_mutex_lock(&historyMutex);
    for (HistoryRecord &record : historyRecords)
    {
        if (record.msg_id == msg_id)
        {
            record.delivered = true;
            break;
        }
    }
    flushHistoryToFile();
    pthread_mutex_unlock(&historyMutex);
}

std::string formatHistoryLine(const HistoryRecord &record)
{
    std::ostringstream line;
    line << "[" << formatTime(record.timestamp) << "]";
    line << "[id=" << record.msg_id << "]";

    if (record.type == MSG_PRIVATE)
    {
        if (record.is_offline)
            line << "[OFFLINE]";
        else
            line << "[PRIVATE]";
        line << "[" << record.sender << " -> " << record.receiver << "]: " << record.text;
    }
    else if (record.type == MSG_SERVER_INFO)
    {
        line << "[SERVER]: " << record.text;
    }
    else
    {
        line << "[" << record.sender << "]: " << record.text;
    }

    return line.str();
}

std::string buildLiveTextLine(uint32_t msg_id, time_t timestamp, const std::string &sender, const std::string &text)
{
    std::ostringstream line;
    line << "[" << formatTime(timestamp) << "]";
    line << "[id=" << msg_id << "]";
    line << "[" << sender << "]: " << text;
    return line.str();
}

std::string buildPrivateLine(uint32_t msg_id,
                             time_t timestamp,
                             const std::string &sender,
                             const std::string &receiver,
                             const std::string &text,
                             bool isOffline)
{
    std::ostringstream line;
    line << "[" << formatTime(timestamp) << "]";
    line << "[id=" << msg_id << "]";
    line << (isOffline ? "[OFFLINE]" : "[PRIVATE]");
    line << "[" << sender << " -> " << receiver << "]: " << text;
    return line.str();
}

std::string getLocalIp(int sock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        return inet_ntoa(addr.sin_addr);
    return "0.0.0.0";
}

std::string getPeerIp(int sock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        return inet_ntoa(addr.sin_addr);
    return "0.0.0.0";
}

uint16_t getLocalPort(int sock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        return ntohs(addr.sin_port);
    return 0;
}

uint16_t getPeerPort(int sock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        return ntohs(addr.sin_port);
    return 0;
}

void logIncoming(int sock, const MessageEx &msg)
{
    std::cout << "[Network Access] frame received via network interface" << std::endl;
    std::cout << "[Internet] src=" << getPeerIp(sock)
              << " dst=" << getLocalIp(sock)
              << " proto=TCP" << std::endl;
    std::cout << "[Transport] recv() " << sizeof(MessageEx) << " bytes via TCP"
              << " src_port=" << getPeerPort(sock)
              << " dst_port=" << getLocalPort(sock) << std::endl;
    std::cout << "[Application] deserialize MessageEx -> " << typeToString(msg.type);
    if (std::strlen(msg.sender) > 0)
        std::cout << " from " << msg.sender;
    std::cout << std::endl;
}

void logOutgoing(int sock, const MessageEx &msg, const std::string &appInfo)
{
    std::cout << "[Application] prepare " << typeToString(msg.type) << " " << appInfo << std::endl;
    std::cout << "[Transport] send() via TCP" << std::endl;
    std::cout << "[Internet] destination ip = " << getPeerIp(sock) << std::endl;
    std::cout << "[Network Access] frame sent to network interface" << std::endl;
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

MessageEx makeMessage(uint8_t type,
                      const std::string &sender,
                      const std::string &receiver,
                      const std::string &payload,
                      uint32_t msg_id,
                      time_t timestamp)
{
    MessageEx msg{};
    msg.type = type;
    msg.msg_id = msg_id;
    msg.timestamp = timestamp;

    std::strncpy(msg.sender, sender.c_str(), MAX_NAME - 1);
    std::strncpy(msg.receiver, receiver.c_str(), MAX_NAME - 1);
    std::strncpy(msg.payload, payload.c_str(), MAX_PAYLOAD - 1);

    uint32_t usefulLength = static_cast<uint32_t>(sizeof(msg.type) + sizeof(msg.msg_id) + sizeof(msg.timestamp) +
                                                  std::strlen(msg.sender) + std::strlen(msg.receiver) + std::strlen(msg.payload));
    msg.length = htonl(usefulLength);
    return msg;
}

bool sendMessageEx(int sock,
                   uint8_t type,
                   const std::string &sender,
                   const std::string &receiver,
                   const std::string &payload,
                   const std::string &appInfo,
                   uint32_t msg_id = 0,
                   time_t timestamp = 0)
{
    if (msg_id == 0)
        msg_id = getNextMessageId();
    if (timestamp == 0)
        timestamp = std::time(nullptr);

    MessageEx msg = makeMessage(type, sender, receiver, payload, msg_id, timestamp);
    logOutgoing(sock, msg, appInfo);
    return sendAll(sock, &msg, sizeof(msg));
}

bool recvMessageEx(int sock, MessageEx &msg)
{
    if (!recvAll(sock, &msg, sizeof(msg)))
        return false;

    msg.length = ntohl(msg.length);
    logIncoming(sock, msg);
    return true;
}

std::string clientNickname(const Client &client)
{
    return client.nickname;
}

bool nicknameExists(const std::string &nickname)
{
    for (const Client &client : clients)
    {
        if (client.authenticated && nickname == client.nickname)
            return true;
    }
    return false;
}

bool addClient(int sock, const std::string &nickname)
{
    pthread_mutex_lock(&clientsMutex);

    if (nicknameExists(nickname))
    {
        pthread_mutex_unlock(&clientsMutex);
        return false;
    }

    Client client{};
    client.sock = sock;
    client.authenticated = 1;
    std::strncpy(client.nickname, nickname.c_str(), MAX_NAME - 1);
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

bool getClientByNickname(const std::string &nickname, Client &result)
{
    pthread_mutex_lock(&clientsMutex);
    for (const Client &client : clients)
    {
        if (client.authenticated && nickname == client.nickname)
        {
            result = client;
            pthread_mutex_unlock(&clientsMutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clientsMutex);
    return false;
}

void sendServerInfoLine(int sock, const std::string &text)
{
    std::string payload = "[SERVER]: " + text;
    sendMessageEx(sock, MSG_SERVER_INFO, "SERVER", "", payload, "server info reply");
}

void broadcastServerInfo(const std::string &text, int excludeSock = -1)
{
    std::vector<int> recipients;

    pthread_mutex_lock(&clientsMutex);
    for (const Client &client : clients)
    {
        if (client.authenticated && client.sock != excludeSock)
            recipients.push_back(client.sock);
    }
    pthread_mutex_unlock(&clientsMutex);

    for (int sock : recipients)
        sendServerInfoLine(sock, text);
}

void broadcastText(uint32_t msg_id, time_t timestamp, const std::string &sender, const std::string &text)
{
    std::string line = buildLiveTextLine(msg_id, timestamp, sender, text);
    std::vector<int> recipients;

    pthread_mutex_lock(&clientsMutex);
    for (const Client &client : clients)
    {
        if (client.authenticated)
            recipients.push_back(client.sock);
    }
    pthread_mutex_unlock(&clientsMutex);

    for (int sock : recipients)
        sendMessageEx(sock, MSG_TEXT, sender, "", line, "broadcast chat message", msg_id, timestamp);
}

bool parsePrivatePayload(const std::string &payload, std::string &target, std::string &text)
{
    size_t pos = payload.find(':');
    if (pos == std::string::npos)
        return false;

    target = payload.substr(0, pos);
    text = payload.substr(pos + 1);
    return !trim(target).empty() && !trim(text).empty();
}

void queueOfflineMessage(uint32_t msg_id,
                         time_t timestamp,
                         const std::string &sender,
                         const std::string &receiver,
                         const std::string &text)
{
    OfflineMsg message{};
    std::strncpy(message.sender, sender.c_str(), MAX_NAME - 1);
    std::strncpy(message.receiver, receiver.c_str(), MAX_NAME - 1);
    std::strncpy(message.text, text.c_str(), MAX_PAYLOAD - 1);
    message.timestamp = timestamp;
    message.msg_id = msg_id;

    pthread_mutex_lock(&offlineMutex);
    offlineQueue.push_back(message);
    pthread_mutex_unlock(&offlineMutex);

    appendHistoryRecord(msg_id, timestamp, sender, receiver, MSG_PRIVATE, text, false, true);
}

void deliverOfflineMessages(int sock, const std::string &nickname)
{
    std::vector<OfflineMsg> pending;

    pthread_mutex_lock(&offlineMutex);
    auto it = offlineQueue.begin();
    while (it != offlineQueue.end())
    {
        if (nickname == it->receiver)
        {
            pending.push_back(*it);
            it = offlineQueue.erase(it);
        }
        else
        {
            ++it;
        }
    }
    pthread_mutex_unlock(&offlineMutex);

    if (pending.empty())
    {
        sendServerInfoLine(sock, "No offline messages for " + nickname);
        return;
    }

    for (const OfflineMsg &offline : pending)
    {
        std::string sender = offline.sender;
        std::string receiver = offline.receiver;
        std::string text = offline.text;
        std::string formatted = buildPrivateLine(offline.msg_id, offline.timestamp, sender, receiver, text, true);

        if (sendMessageEx(sock,
                          MSG_PRIVATE,
                          sender,
                          receiver,
                          formatted,
                          "deliver offline private message",
                          offline.msg_id,
                          offline.timestamp))
        {
            markHistoryDelivered(offline.msg_id);
            sendServerInfoLine(sock, "message delivered (maybe)");
        }
    }
}

void handleListRequest(int sock)
{
    sendServerInfoLine(sock, "Online users");

    pthread_mutex_lock(&clientsMutex);
    std::vector<std::string> names;
    for (const Client &client : clients)
    {
        if (client.authenticated)
            names.push_back(client.nickname);
    }
    pthread_mutex_unlock(&clientsMutex);

    if (names.empty())
    {
        sendServerInfoLine(sock, "<no users>");
        return;
    }

    std::sort(names.begin(), names.end());
    for (const std::string &name : names)
        sendServerInfoLine(sock, name);
}

void handleHistoryRequest(int sock, const std::string &payload)
{
    int count = DEFAULT_HISTORY_COUNT;
    std::string trimmed = trim(payload);
    if (!trimmed.empty())
    {
        try
        {
            count = std::stoi(trimmed);
        }
        catch (...)
        {
            sendMessageEx(sock, MSG_ERROR, "SERVER", "", "[SERVER][ERROR]: invalid history count", "history error");
            return;
        }

        if (count <= 0)
        {
            sendMessageEx(sock, MSG_ERROR, "SERVER", "", "[SERVER][ERROR]: history count must be positive", "history error");
            return;
        }
    }

    pthread_mutex_lock(&historyMutex);
    if (historyRecords.empty())
    {
        pthread_mutex_unlock(&historyMutex);
        sendServerInfoLine(sock, "History is empty");
        return;
    }

    int start = static_cast<int>(historyRecords.size()) - count;
    if (start < 0)
        start = 0;

    std::vector<HistoryRecord> chunk(historyRecords.begin() + start, historyRecords.end());
    pthread_mutex_unlock(&historyMutex);

    for (const HistoryRecord &record : chunk)
    {
        std::string line = formatHistoryLine(record);
        sendMessageEx(sock,
                      MSG_HISTORY_DATA,
                      record.sender,
                      record.receiver,
                      line,
                      "history data reply",
                      record.msg_id,
                      record.timestamp);
    }
}

void handleClientMessages(int clientSock, const std::string &nickname)
{
    while (true)
    {
        MessageEx msg{};
        if (!recvMessageEx(clientSock, msg))
            break;

        std::cout << "[Application] handle " << typeToString(msg.type) << std::endl;

        if (msg.type == MSG_TEXT)
        {
            std::string text = trim(msg.payload);
            if (text.empty())
                continue;

            uint32_t msgId = getNextMessageId();
            time_t timestamp = std::time(nullptr);
            appendHistoryRecord(msgId, timestamp, nickname, "", MSG_TEXT, text, true, false);
            broadcastText(msgId, timestamp, nickname, text);
        }
        else if (msg.type == MSG_PRIVATE)
        {
            std::string target;
            std::string text;
            if (!parsePrivatePayload(msg.payload, target, text))
            {
                sendMessageEx(clientSock, MSG_ERROR, "SERVER", "", "[SERVER][ERROR]: invalid private message format", "private message error");
                continue;
            }

            Client targetClient{};
            uint32_t msgId = getNextMessageId();
            time_t timestamp = std::time(nullptr);

            if (getClientByNickname(trim(target), targetClient))
            {
                std::string formatted = buildPrivateLine(msgId, timestamp, nickname, trim(target), text, false);
                sendMessageEx(targetClient.sock,
                              MSG_PRIVATE,
                              nickname,
                              trim(target),
                              formatted,
                              "deliver private message",
                              msgId,
                              timestamp);
                sendServerInfoLine(clientSock, "private message sent");
                appendHistoryRecord(msgId, timestamp, nickname, trim(target), MSG_PRIVATE, text, true, false);
            }
            else
            {
                queueOfflineMessage(msgId, timestamp, nickname, trim(target), text);
                sendServerInfoLine(clientSock, "receiver " + trim(target) + " is offline, message stored");
            }
        }
        else if (msg.type == MSG_LIST)
        {
            handleListRequest(clientSock);
        }
        else if (msg.type == MSG_HISTORY)
        {
            handleHistoryRequest(clientSock, msg.payload);
        }
        else if (msg.type == MSG_PING)
        {
            sendMessageEx(clientSock, MSG_PONG, "SERVER", "", "[SERVER]: PONG", "ping response");
        }
        else if (msg.type == MSG_BYE)
        {
            break;
        }
    }
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

        MessageEx msg{};
        if (!recvMessageEx(clientSock, msg))
        {
            close(clientSock);
            continue;
        }

        std::cout << "[Application] handle " << typeToString(msg.type) << std::endl;

        if (msg.type != MSG_HELLO)
        {
            sendMessageEx(clientSock, MSG_ERROR, "SERVER", "", "[SERVER][ERROR]: HELLO expected", "reject connection without HELLO");
            close(clientSock);
            continue;
        }

        sendMessageEx(clientSock, MSG_WELCOME, "SERVER", "", "[SERVER]: Service handshake completed", "welcome after HELLO");

        bool authenticated = false;
        std::string nickname;

        while (!authenticated)
        {
            if (!recvMessageEx(clientSock, msg))
                break;

            if (msg.type != MSG_AUTH)
            {
                std::cout << "[Application] ignore " << typeToString(msg.type) << " before authentication" << std::endl;
                continue;
            }

            nickname = trim(msg.payload);
            if (nickname.empty())
            {
                std::cout << "[Application] authentication failed: empty nickname" << std::endl;
                sendMessageEx(clientSock, MSG_ERROR, "SERVER", "", "[SERVER][ERROR]: nickname must not be empty", "authentication error");
                close(clientSock);
                clientSock = -1;
                break;
            }

            if (!addClient(clientSock, nickname))
            {
                std::cout << "[Application] authentication failed: duplicate nickname" << std::endl;
                sendMessageEx(clientSock, MSG_ERROR, "SERVER", "", "[SERVER][ERROR]: nickname already in use", "authentication error");
                close(clientSock);
                clientSock = -1;
                break;
            }

            authenticated = true;
            std::cout << "[Application] authentication success: " << nickname << std::endl;
            sendServerInfoLine(clientSock, "Authentication success: " + nickname);
            deliverOfflineMessages(clientSock, nickname);

            std::string info = "User [" + nickname + "] connected";
            std::cout << info << std::endl;
            appendHistoryRecord(getNextMessageId(), std::time(nullptr), "SERVER", "", MSG_SERVER_INFO, info, true, false);
            broadcastServerInfo(info, -1);
        }

        if (clientSock < 0 || !authenticated)
            continue;

        handleClientMessages(clientSock, nickname);

        Client removed{};
        if (removeClient(clientSock, removed))
        {
            std::string info = "User [" + clientNickname(removed) + "] disconnected";
            std::cout << info << std::endl;
            appendHistoryRecord(getNextMessageId(), std::time(nullptr), "SERVER", "", MSG_SERVER_INFO, info, true, false);
            broadcastServerInfo(info, clientSock);
        }

        close(clientSock);
    }

    return nullptr;
}

int main()
{
    loadHistoryFromFile();

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
    {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    {
        std::perror("bind");
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, 10) < 0)
    {
        std::perror("listen");
        close(serverFd);
        return 1;
    }

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i)
        pthread_create(&threads[i], nullptr, worker, nullptr);

    std::cout << "Server started on port " << PORT << std::endl;

    while (true)
    {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        int clientSock = accept(serverFd, reinterpret_cast<sockaddr *>(&clientAddr), &len);
        if (clientSock < 0)
            continue;

        pthread_mutex_lock(&queueMutex);
        clientQueue.push(clientSock);
        pthread_cond_signal(&queueCond);
        pthread_mutex_unlock(&queueMutex);
    }

    close(serverFd);
    return 0;
}
