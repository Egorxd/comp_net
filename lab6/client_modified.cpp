#include <arpa/inet.h>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <pthread.h>

#include "message.h"

static const int ACK_TIMEOUT_MS = 2000;
static const int MAX_RETRIES = 3;
static const int DEFAULT_PING_COUNT = 10;
static const int PING_TIMEOUT_MS = 2000;

int sock = -1;
bool running = true;
std::string currentNickname;
uint32_t nextLocalMessageId = 1;

pthread_mutex_t sendMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t coutMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pendingMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pingMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t idMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t pingCond = PTHREAD_COND_INITIALIZER;

struct PendingMsg
{
    MessageEx msg;
    std::chrono::steady_clock::time_point send_time;
    int retries;
};

struct PingRecord
{
    uint32_t msg_id;
    int index;
    std::chrono::steady_clock::time_point send_time;
    bool responded;
    bool timed_out;
    double rtt_ms;
    double jitter_ms;
    bool has_jitter;
};

struct NetDiagStats
{
    int sent = 0;
    int received = 0;
    int lost = 0;
    double rtt_avg_ms = 0.0;
    double jitter_avg_ms = 0.0;
    double loss_percent = 0.0;
    std::vector<PingRecord> samples;
};

std::vector<PendingMsg> pendingMsgs;
std::vector<PingRecord> pingRecords;
NetDiagStats lastStats;
double lastSuccessfulPingRtt = -1.0;

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
        case MSG_ACK: return "MSG_ACK";
        default: return "UNKNOWN";
    }
}

void printLine(const std::string &line)
{
    pthread_mutex_lock(&coutMutex);
    std::cout << line << std::endl;
    pthread_mutex_unlock(&coutMutex);
}

uint32_t getNextLocalMessageId()
{
    pthread_mutex_lock(&idMutex);
    uint32_t id = nextLocalMessageId++;
    pthread_mutex_unlock(&idMutex);
    return id;
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
                      const std::string &payload,
                      uint32_t msgId = 0)
{
    MessageEx msg{};
    msg.type = type;
    msg.msg_id = (msgId == 0) ? getNextLocalMessageId() : msgId;
    msg.timestamp = std::time(nullptr);

    std::strncpy(msg.sender, sender.c_str(), MAX_NAME - 1);
    std::strncpy(msg.receiver, receiver.c_str(), MAX_NAME - 1);
    std::strncpy(msg.payload, payload.c_str(), MAX_PAYLOAD - 1);

    uint32_t usefulLength = static_cast<uint32_t>(sizeof(msg.type) + sizeof(msg.msg_id) + sizeof(msg.timestamp) +
                                                  std::strlen(msg.sender) + std::strlen(msg.receiver) + std::strlen(msg.payload));
    msg.length = htonl(usefulLength);
    return msg;
}

bool sendRawMessage(const MessageEx &msg)
{
    pthread_mutex_lock(&sendMutex);
    bool ok = sendAll(sock, &msg, sizeof(msg));
    pthread_mutex_unlock(&sendMutex);
    return ok;
}

bool sendMessageEx(uint8_t type, const std::string &receiver, const std::string &payload, const std::string &sender = "")
{
    MessageEx msg = makeMessage(type, sender, receiver, payload);
    return sendRawMessage(msg);
}

bool recvMessageEx(MessageEx &msg)
{
    if (!recvAll(sock, &msg, sizeof(msg)))
        return false;

    msg.length = ntohl(msg.length);
    return true;
}

bool isReliableClientMessage(uint8_t type)
{
    return type == MSG_TEXT || type == MSG_PRIVATE || type == MSG_PING;
}

bool sendReliablePrepared(const MessageEx &msg)
{
    pthread_mutex_lock(&pendingMutex);
    pendingMsgs.push_back({msg, std::chrono::steady_clock::now(), 0});
    pthread_mutex_unlock(&pendingMutex);

    printLine("[Transport][RETRY] send " + typeToString(msg.type) + " (id=" + std::to_string(msg.msg_id) + ")");
    return sendRawMessage(msg);
}

bool sendReliableMessage(uint8_t type, const std::string &receiver, const std::string &payload, const std::string &sender)
{
    MessageEx msg = makeMessage(type, sender, receiver, payload);
    return sendReliablePrepared(msg);
}

void handleAck(uint32_t msgId)
{
    bool found = false;

    pthread_mutex_lock(&pendingMutex);
    for (auto it = pendingMsgs.begin(); it != pendingMsgs.end(); ++it)
    {
        if (it->msg.msg_id == msgId)
        {
            pendingMsgs.erase(it);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&pendingMutex);

    if (found)
        printLine("[Transport][RETRY] ACK received (id=" + std::to_string(msgId) + ")");
}

void retryWorker()
{
    while (running)
    {
        usleep(100 * 1000);

        std::vector<MessageEx> toResend;
        auto now = std::chrono::steady_clock::now();

        pthread_mutex_lock(&pendingMutex);
        for (auto it = pendingMsgs.begin(); it != pendingMsgs.end();)
        {
            long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->send_time).count();
            if (elapsed < ACK_TIMEOUT_MS)
            {
                ++it;
                continue;
            }

            printLine("[Transport][RETRY] wait ACK timeout (id=" + std::to_string(it->msg.msg_id) + ")");

            if (it->retries >= MAX_RETRIES)
            {
                printLine("[Transport][RETRY] delivery failed after 3 retries (id=" + std::to_string(it->msg.msg_id) + ")");
                it = pendingMsgs.erase(it);
                continue;
            }

            it->retries += 1;
            it->send_time = now;
            toResend.push_back(it->msg);
            printLine("[Transport][RETRY] resend " + std::to_string(it->retries) + "/3 (id=" + std::to_string(it->msg.msg_id) + ")");
            ++it;
        }
        pthread_mutex_unlock(&pendingMutex);

        for (const MessageEx &msg : toResend)
            sendRawMessage(msg);
    }
}

void handlePong(const MessageEx &msg)
{
    auto now = std::chrono::steady_clock::now();
    std::ostringstream out;
    bool handled = false;

    pthread_mutex_lock(&pingMutex);
    for (PingRecord &record : pingRecords)
    {
        if (record.msg_id == msg.msg_id)
        {
            if (!record.responded && !record.timed_out)
            {
                record.rtt_ms = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - record.send_time).count()) / 1000.0;
                record.responded = true;

                if (lastSuccessfulPingRtt >= 0.0)
                {
                    record.jitter_ms = std::fabs(record.rtt_ms - lastSuccessfulPingRtt);
                    record.has_jitter = true;
                }
                lastSuccessfulPingRtt = record.rtt_ms;

                out << std::fixed << std::setprecision(1);
                out << "PING " << record.index << " -> RTT=" << record.rtt_ms << "ms";
                if (record.has_jitter)
                    out << " | Jitter=" << record.jitter_ms << "ms";
                handled = true;
            }
            break;
        }
    }
    pthread_cond_broadcast(&pingCond);
    pthread_mutex_unlock(&pingMutex);

    if (handled)
        printLine(out.str());
}

NetDiagStats calculateStatsLocked()
{
    NetDiagStats stats;
    stats.samples = pingRecords;
    stats.sent = static_cast<int>(pingRecords.size());

    double rttSum = 0.0;
    double jitterSum = 0.0;
    int jitterCount = 0;

    for (const PingRecord &record : pingRecords)
    {
        if (record.responded)
        {
            stats.received += 1;
            rttSum += record.rtt_ms;
            if (record.has_jitter)
            {
                jitterSum += record.jitter_ms;
                jitterCount += 1;
            }
        }
    }

    stats.lost = stats.sent - stats.received;
    if (stats.received > 0)
        stats.rtt_avg_ms = rttSum / stats.received;
    if (jitterCount > 0)
        stats.jitter_avg_ms = jitterSum / jitterCount;
    if (stats.sent > 0)
        stats.loss_percent = static_cast<double>(stats.lost) * 100.0 / static_cast<double>(stats.sent);

    return stats;
}

void performPing(int count)
{
    if (count <= 0)
    {
        printLine("Usage: /ping N, where N > 0");
        return;
    }

    pthread_mutex_lock(&pingMutex);
    pingRecords.clear();
    lastSuccessfulPingRtt = -1.0;
    lastStats = NetDiagStats{};
    pthread_mutex_unlock(&pingMutex);

    for (int i = 1; i <= count; ++i)
    {
        MessageEx msg = makeMessage(MSG_PING, currentNickname, "", "PING");

        pthread_mutex_lock(&pingMutex);
        pingRecords.push_back({msg.msg_id, i, std::chrono::steady_clock::now(), false, false, 0.0, 0.0, false});
        pthread_mutex_unlock(&pingMutex);

        if (!sendReliablePrepared(msg))
        {
            printLine("[Transport][PING] send failed");
            return;
        }
        usleep(50 * 1000);
    }

    bool finished = false;
    while (!finished && running)
    {
        std::vector<int> timedOutIndexes;
        auto now = std::chrono::steady_clock::now();

        pthread_mutex_lock(&pingMutex);
        finished = true;
        for (PingRecord &record : pingRecords)
        {
            if (record.responded || record.timed_out)
                continue;

            long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - record.send_time).count();
            if (elapsed >= PING_TIMEOUT_MS)
            {
                record.timed_out = true;
                timedOutIndexes.push_back(record.index);
            }
            else
            {
                finished = false;
            }
        }
        pthread_mutex_unlock(&pingMutex);

        for (int index : timedOutIndexes)
            printLine("PING " + std::to_string(index) + " -> timeout");

        if (!finished)
            usleep(50 * 1000);
    }

    pthread_mutex_lock(&pingMutex);
    lastStats = calculateStatsLocked();
    pthread_mutex_unlock(&pingMutex);
}

void saveNetDiagJson(const NetDiagStats &stats, const std::string &filename)
{
    std::ofstream out(filename, std::ios::trunc);
    out << std::fixed << std::setprecision(3);
    out << "{\n";
    out << "  \"nickname\": \"" << escapeJson(currentNickname) << "\",\n";
    out << "  \"sent\": " << stats.sent << ",\n";
    out << "  \"received\": " << stats.received << ",\n";
    out << "  \"lost\": " << stats.lost << ",\n";
    out << "  \"rtt_avg_ms\": " << stats.rtt_avg_ms << ",\n";
    out << "  \"jitter_avg_ms\": " << stats.jitter_avg_ms << ",\n";
    out << "  \"loss_percent\": " << stats.loss_percent << ",\n";
    out << "  \"samples\": [\n";

    for (size_t i = 0; i < stats.samples.size(); ++i)
    {
        const PingRecord &record = stats.samples[i];
        out << "    {\n";
        out << "      \"index\": " << record.index << ",\n";
        out << "      \"msg_id\": " << record.msg_id << ",\n";
        out << "      \"received\": " << (record.responded ? "true" : "false") << ",\n";
        out << "      \"timeout\": " << (record.timed_out ? "true" : "false") << ",\n";
        out << "      \"rtt_ms\": " << record.rtt_ms << ",\n";
        out << "      \"jitter_ms\": " << record.jitter_ms << "\n";
        out << "    }";
        if (i + 1 != stats.samples.size())
            out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";
}

void printNetDiag()
{
    pthread_mutex_lock(&pingMutex);
    NetDiagStats stats = lastStats;
    pthread_mutex_unlock(&pingMutex);

    if (stats.sent == 0)
    {
        printLine("No diagnostics yet. Run /ping or /ping N first.");
        return;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "RTT avg : " << stats.rtt_avg_ms << " ms\n";
    out << "Jitter  : " << stats.jitter_avg_ms << " ms\n";
    out << "Loss    : " << stats.loss_percent << "%";
    printLine(out.str());

    std::string filename = "net_diag_" + currentNickname + ".json";
    saveNetDiagJson(stats, filename);
    printLine("Saved diagnostics to " + filename);
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
    printLine("Available commands:\n"
              "/help\n"
              "/list\n"
              "/history\n"
              "/history N\n"
              "/quit\n"
              "/w <nick> <message>\n"
              "/ping\n"
              "/ping N\n"
              "/netdiag\n"
              "Tip: packets now can retry");
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

    printLine(msg.payload);

    if (!sendMessageEx(MSG_AUTH, "", nickname, nickname))
        return false;

    if (!recvMessageEx(msg))
        return false;

    if (msg.type == MSG_ERROR)
    {
        printLine(msg.payload);
        return false;
    }

    if (msg.type == MSG_SERVER_INFO)
        printLine(msg.payload);

    printLine("Connected");
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
                printLine("Disconnected");
            running = false;
            if (sock >= 0)
                close(sock);
            break;
        }

        if (msg.type == MSG_ACK)
        {
            handleAck(msg.msg_id);
        }
        else if (msg.type == MSG_PONG)
        {
            handlePong(msg);
        }
        else if (std::strlen(msg.payload) > 0)
        {
            printLine(msg.payload);
        }
    }
}

bool parsePositiveInt(const std::string &value, int &result)
{
    std::string trimmed = trim(value);
    if (trimmed.empty())
        return false;

    for (char ch : trimmed)
    {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    }

    try
    {
        result = std::stoi(trimmed);
    }
    catch (...)
    {
        return false;
    }

    return result > 0;
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
        printLine("Reconnect...");
        sleep(2);
    }

    std::thread receiverThread(receiver);
    std::thread retryThread(retryWorker);
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
            int ignored = 0;
            if (!parsePositiveInt(amount, ignored))
            {
                printLine("Usage: /history N");
                continue;
            }

            if (!sendMessageEx(MSG_HISTORY, "", amount, currentNickname))
                break;
        }
        else if (input == "/ping")
        {
            performPing(DEFAULT_PING_COUNT);
        }
        else if (input.rfind("/ping ", 0) == 0)
        {
            int count = 0;
            if (!parsePositiveInt(input.substr(6), count))
            {
                printLine("Usage: /ping N");
                continue;
            }
            performPing(count);
        }
        else if (input == "/netdiag")
        {
            printNetDiag();
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
                printLine("Usage: /w <nick> <message> or /w \"nick with spaces\" <message>");
                continue;
            }

            if (!sendReliableMessage(MSG_PRIVATE, target, target + ":" + text, currentNickname))
                break;
        }
        else
        {
            if (!sendReliableMessage(MSG_TEXT, "", input, currentNickname))
                break;
        }
    }

    running = false;
    if (sock >= 0)
        shutdown(sock, SHUT_RDWR);

    receiverThread.join();
    retryThread.join();
    return 0;
}
