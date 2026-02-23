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

    sockaddr_in serverAddr{}, clientAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    std::cout << "UDP Echo Server started on port 8080\n";

    char buffer[1024];
    socklen_t addrLen = sizeof(clientAddr);

    while (true) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                         (struct sockaddr*)&clientAddr, &addrLen);

        if (n < 0) {
            perror("recvfrom");
            continue;
        }

        buffer[n] = '\0';

        std::cout << "Client ["
                  << inet_ntoa(clientAddr.sin_addr)
                  << ":" << ntohs(clientAddr.sin_port)
                  << "] -> " << buffer << std::endl;

        sendto(sockfd, buffer, n, 0,
               (struct sockaddr*)&clientAddr, addrLen);
    }

    close(sockfd);
    return 0;
}
