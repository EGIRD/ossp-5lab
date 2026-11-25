#include "../common/protocol.h"
#include "../common/logger.h"
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <iomanip>

double getHighResMs(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq) {
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

std::string unescapePayload(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 20001;
    std::string clientId = "1";
    int timeoutMs = 5000;
    int count = 0;
    std::string autoCmd = "SIZE small.txt";
    std::string csvOut = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--id" && i + 1 < argc) clientId = argv[++i];
        else if (arg == "--timeout" && i + 1 < argc) timeoutMs = std::stoi(argv[++i]);
        else if (arg == "--count" && i + 1 < argc) count = std::stoi(argv[++i]);
        else if (arg == "--cmd" && i + 1 < argc) autoCmd = argv[++i];
        else if (arg == "--csv" && i + 1 < argc) csvOut = argv[++i];
    }

    Logger::init("client_" + clientId + ".log");

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        netproto::printWsaError("WSAStartup");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        netproto::printWsaError("socket");
        WSACleanup();
        return 1;
    }

    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srvAddr{};
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &srvAddr.sin_addr);

    LARGE_INTEGER freq{}, tConnStart{}, tConnEnd{};
    QueryPerformanceFrequency(&freq);

    std::string peer = host + ":" + std::to_string(port);
    QueryPerformanceCounter(&tConnStart);
    if (connect(sock, reinterpret_cast<sockaddr*>(&srvAddr), sizeof(srvAddr)) == SOCKET_ERROR) {
        netproto::printWsaError("connect");
        Logger::log("CLIENT", clientId, peer, "CONNECT", "-", "FAILED");
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    QueryPerformanceCounter(&tConnEnd);
    double connectMs = getHighResMs(tConnStart, tConnEnd, freq);
    Logger::log("CLIENT", clientId, peer, "CONNECT", "-", "OK (" + std::to_string(connectMs) + " ms)");

    uint64_t reqSeq = 1;
    size_t okCount = 0, errCount = 0;
    size_t totalBytesSent = 0, totalBytesRecv = 0;

    auto executeOne = [&](const std::string& commandLine, double& rttMs) -> bool {
        std::string reqId = clientId + "_" + std::to_string(reqSeq++);
        std::string packet = "REQ|1.0|" + clientId + "|" + reqId + "|" + commandLine;

        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);

        if (!netproto::sendFrame(sock, packet)) {
            netproto::printWsaError("sendFrame");
            Logger::log("CLIENT", clientId, peer, "SEND", reqId, "ERROR");
            return false;
        }
        totalBytesSent += packet.size() + 4;

        std::string response;
        int st = netproto::recvFrame(sock, response);
        QueryPerformanceCounter(&t1);
        rttMs = getHighResMs(t0, t1, freq);

        if (st <= 0) {
            if (st == -1) netproto::printWsaError("recvFrame");
            Logger::log("CLIENT", clientId, peer, "RECV", reqId, st == 0 ? "CLOSED" : "ERROR");
            return false;
        }
        totalBytesRecv += response.size() + 4;

        size_t p1 = response.find('|');
        size_t p2 = (p1 != std::string::npos) ? response.find('|', p1 + 1) : std::string::npos;
        size_t p3 = (p2 != std::string::npos) ? response.find('|', p2 + 1) : std::string::npos;
        size_t p4 = (p3 != std::string::npos) ? response.find('|', p3 + 1) : std::string::npos;

        if (p4 == std::string::npos) {
            std::cout << "[RAW RESPONSE] " << response << "\n";
            ++errCount;
            return true;
        }

        std::string status = response.substr(p3 + 1, p4 - p3 - 1);
        std::string data = unescapePayload(response.substr(p4 + 1));

        if (status == "OK") ++okCount;
        else ++errCount;

        if (count == 0) {
            std::cout << "[" << status << "] (" << std::fixed << std::setprecision(3) << rttMs << " ms):\n"
                      << data << "\n";
        }
        return true;
    };

    if (count > 0) {
        LARGE_INTEGER expStart{}, expEnd{};
        QueryPerformanceCounter(&expStart);
        double sumRtt = 0.0;

        for (int i = 0; i < count; ++i) {
            double rtt = 0.0;
            if (!executeOne(autoCmd, rtt)) break;
            sumRtt += rtt;
        }
        QueryPerformanceCounter(&expEnd);
        double totalMs = getHighResMs(expStart, expEnd, freq);
        double avgRtt = (okCount + errCount) > 0 ? sumRtt / (okCount + errCount) : 0.0;
        double throughputOps = (totalMs > 0) ? ((okCount + errCount) * 1000.0 / totalMs) : 0.0;

        std::cout << "=== EXPERIMENT SUMMARY (client=" << clientId << ") ===\n"
                  << "Command:     " << autoCmd << "\n"
                  << "Total time:  " << totalMs << " ms\n"
                  << "Avg RTT:     " << avgRtt << " ms\n"
                  << "Throughput:  " << throughputOps << " req/s\n"
                  << "OK / ERR:    " << okCount << " / " << errCount << "\n"
                  << "Bytes TX/RX: " << totalBytesSent << " / " << totalBytesRecv << "\n";

        if (!csvOut.empty()) {
            bool writeHeader = !std::ifstream(csvOut).good();
            std::ofstream csv(csvOut, std::ios::app);
            if (writeHeader) {
                csv << "client_id,command,requests,total_ms,avg_rtt_ms,throughput_rps,ok,err,bytes_tx,bytes_rx\n";
            }
            csv << clientId << ",\"" << autoCmd << "\"," << count << ","
                << totalMs << "," << avgRtt << "," << throughputOps << ","
                << okCount << "," << errCount << "," << totalBytesSent << "," << totalBytesRecv << "\n";
        }
        double dummy = 0.0;
        executeOne("QUIT", dummy);
    } else {
        std::cout << "Connected to " << peer << ". Type HELP for commands, QUIT to exit.\n";
        std::string line;
        while (true) {
            std::cout << "> ";
            if (!std::getline(std::cin, line) || line.empty()) continue;
            double rtt = 0.0;
            if (!executeOne(line, rtt)) break;
            if (line == "QUIT") break;
        }
    }

    shutdown(sock, SD_BOTH);
    closesocket(sock);
    WSACleanup();
    return 0;
}