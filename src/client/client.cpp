#include "../common/protocol.h"
#include "../common/logger.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 20001;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in srvAddr{};
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &srvAddr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&srvAddr), sizeof(srvAddr)) == SOCKET_ERROR) {
        netproto::printWsaError("connect");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line == "QUIT") break;
        netproto::sendFrame(sock, line);
        std::string resp;
        if (netproto::recvFrame(sock, resp) > 0) std::cout << resp << "\n";
        else break;
    }

    shutdown(sock, SD_BOTH);
    closesocket(sock);
    WSACleanup();
    return 0;
}