#include "../common/protocol.h"
#include "../common/logger.h"
#include <thread>
#include <atomic>
#include <csignal>

SOCKET g_listenSock = INVALID_SOCKET;
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
    if (g_listenSock != INVALID_SOCKET) closesocket(g_listenSock);
}

void handleClientBasic(SOCKET clientSock, std::string peerAddr) {
    Logger::log("SERVER", "unknown", peerAddr, "CLIENT_CONNECTED", "-", "OK");
    while (g_running) {
        std::string frame;
        int st = netproto::recvFrame(clientSock, frame);
        if (st <= 0) {
            Logger::log("SERVER", "unknown", peerAddr, "CLIENT_DISCONNECTED", "-", st == 0 ? "CLOSED" : "ERROR");
            break;
        }
        Logger::log("SERVER", "unknown", peerAddr, "RECV", "-", frame);
        netproto::sendFrame(clientSock, "RES|1.0|1|OK|ECHO: " + frame);
    }
    shutdown(clientSock, SD_BOTH);
    closesocket(clientSock);
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 20001;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    std::signal(SIGINT, signalHandler);
    Logger::init("server.log");

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        netproto::printWsaError("WSAStartup");
        return 1;
    }

    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (bind(g_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        netproto::printWsaError("bind");
        closesocket(g_listenSock);
        WSACleanup();
        return 1;
    }
    listen(g_listenSock, SOMAXCONN);
    Logger::log("SERVER", "0", host + ":" + std::to_string(port), "START", "-", "BASIC_MODE");

    while (g_running) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(g_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCKET) break;
        char ipStr[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string peer = std::string(ipStr) + ":" + std::to_string(ntohs(clientAddr.sin_port));
        std::thread(handleClientBasic, clientSock, peer).detach();
    }

    WSACleanup();
    return 0;
}