#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return 1;
    }

    std::cout << "UDP Client started\n";

    char message[1024];
    char buffer[1024];

    while (true) {
        std::cout << "Enter message: ";
        std::cin.getline(message, sizeof(message));

        int sent = sendto(sockfd, message, strlen(message), 0,
                          (struct sockaddr*)&serverAddr,
                          sizeof(serverAddr));

        if (sent < 0) {
            perror("sendto");
            continue;
        }

        socklen_t addrLen = sizeof(serverAddr);
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                         (struct sockaddr*)&serverAddr, &addrLen);

        if (n < 0) {
            perror("recvfrom");
            continue;
        }

        buffer[n] = '\0';
        std::cout << "Server: " << buffer << std::endl;
    }

    close(sockfd);
    return 0;
}
