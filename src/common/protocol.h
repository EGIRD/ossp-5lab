#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

namespace netproto {

constexpr uint32_t MAX_MESSAGE_SIZE = 65536; 
constexpr const char* PROTOCOL_VERSION = "1.0";

inline std::string getWsaErrorText(int errCode) {
    char* msgBuf = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errCode, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&msgBuf), 0, nullptr);
    std::string res = msgBuf ? msgBuf : "Unknown error";
    if (msgBuf) LocalFree(msgBuf);
    while (!res.empty() && (res.back() == '\r' || res.back() == '\n')) res.pop_back();
    return res;
}

inline void printWsaError(const std::string& funcName) {
    int err = WSAGetLastError();
    std::cerr << "[WINSOCK ERROR] " << funcName << " failed with code " << err
              << " (" << getWsaErrorText(err) << ")\n";
}

inline bool sendAll(SOCKET sock, const char* data, int totalBytes) {
    int sent = 0;
    while (sent < totalBytes) {
        int res = send(sock, data + sent, totalBytes - sent, 0);
        if (res == SOCKET_ERROR || res == 0) {
            return false;
        }
        sent += res;
    }
    return true;
}

inline bool recvAll(SOCKET sock, char* buffer, int totalBytes, bool& connectionClosed) {
    connectionClosed = false;
    int received = 0;
    while (received < totalBytes) {
        int res = recv(sock, buffer + received, totalBytes - received, 0);
        if (res == 0) {
            connectionClosed = true;
            return false;
        }
        if (res == SOCKET_ERROR) {
            return false;
        }
        received += res;
    }
    return true;
}

inline bool sendFrame(SOCKET sock, const std::string& payload) {
    if (payload.size() > MAX_MESSAGE_SIZE) return false;
    uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
    if (!sendAll(sock, reinterpret_cast<const char*>(&netLen), sizeof(netLen))) return false;
    if (!payload.empty()) {
        if (!sendAll(sock, payload.data(), static_cast<int>(payload.size()))) return false;
    }
    return true;
}

inline int recvFrame(SOCKET sock, std::string& outPayload) {
    uint32_t netLen = 0;
    bool closed = false;
    if (!recvAll(sock, reinterpret_cast<char*>(&netLen), sizeof(netLen), closed)) {
        return closed ? 0 : -1;
    }
    uint32_t hostLen = ntohl(netLen);
    if (hostLen == 0 || hostLen > MAX_MESSAGE_SIZE) {
        return -2; 
    }
    outPayload.resize(hostLen);
    if (!recvAll(sock, &outPayload[0], static_cast<int>(hostLen), closed)) {
        return closed ? 0 : -1;
    }
    return 1;
}

}