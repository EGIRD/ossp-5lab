#include "../common/protocol.h"
#include "../common/logger.h"
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <csignal>

namespace fs = std::filesystem;

SOCKET g_listenSock = INVALID_SOCKET;
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
    if (g_listenSock != INVALID_SOCKET) {
        closesocket(g_listenSock);
    }
}

// Проверка пути: запрет выхода за пределы sandbox (../, ..\ , абсолютные пути, диски)
bool resolveSandboxPath(const fs::path& sandboxRoot, const std::string& userFilename, fs::path& outResolved) {
    if (userFilename.empty()) return false;
    fs::path rel(userFilename);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        return false;
    }
    std::error_code ec;
    fs::path canonRoot = fs::weakly_canonical(sandboxRoot, ec);
    if (ec) return false;

    fs::path combined = fs::weakly_canonical(canonRoot / rel, ec);
    if (ec) return false;

    auto rootIt = canonRoot.begin();
    auto combIt = combined.begin();
    for (; rootIt != canonRoot.end(); ++rootIt, ++combIt) {
        if (combIt == combined.end() || *combIt != *rootIt) {
            return false;
        }
    }
    if (combined == canonRoot) return false;

    outResolved = combined;
    return true;
}

std::string escapePayload(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out;
}

std::string executeCommand(const fs::path& sandboxRoot, const std::string& cmdLine, bool& isOk) {
    isOk = false;
    std::istringstream iss(cmdLine);
    std::string cmd;
    iss >> cmd;

    if (cmd == "HELP") {
        isOk = true;
        return "Available commands: LIST | SIZE <filename> | COUNT_LINES <filename> | READ_FIRST <filename> <n> | HELP | QUIT";
    }
    else if (cmd == "LIST") {
        std::string extra;
        if (iss >> extra) return "ERR_BAD_ARGS: LIST takes no arguments";
        std::error_code ec;
        std::ostringstream res;
        bool first = true;
        for (const auto& entry : fs::directory_iterator(sandboxRoot, ec)) {
            if (!first) res << "; ";
            res << entry.path().filename().string();
            if (entry.is_directory()) res << "/";
            first = false;
        }
        isOk = true;
        return first ? "(empty)" : res.str();
    }
    else if (cmd == "SIZE") {
        std::string filename, extra;
        if (!(iss >> filename) || (iss >> extra)) {
            return "ERR_BAD_ARGS: Usage: SIZE <filename>";
        }
        fs::path target;
        if (!resolveSandboxPath(sandboxRoot, filename, target)) {
            return "ERR_ACCESS_DENIED: Path escapes sandbox or invalid";
        }
        if (!fs::exists(target) || !fs::is_regular_file(target)) {
            return "ERR_FILE_NOT_FOUND: File does not exist";
        }
        std::error_code ec;
        auto sz = fs::file_size(target, ec);
        if (ec) return "ERR_IO: Cannot read file size";
        isOk = true;
        return std::to_string(sz) + " bytes";
    }
    else if (cmd == "COUNT_LINES") {
        std::string filename, extra;
        if (!(iss >> filename) || (iss >> extra)) {
            return "ERR_BAD_ARGS: Usage: COUNT_LINES <filename>";
        }
        fs::path target;
        if (!resolveSandboxPath(sandboxRoot, filename, target)) {
            return "ERR_ACCESS_DENIED: Path escapes sandbox or invalid";
        }
        if (!fs::exists(target) || !fs::is_regular_file(target)) {
            return "ERR_FILE_NOT_FOUND: File does not exist";
        }
        std::ifstream fin(target);
        if (!fin.is_open()) return "ERR_IO: Cannot open file";
        size_t lines = 0;
        std::string line;
        while (std::getline(fin, line)) ++lines;
        isOk = true;
        return std::to_string(lines) + " lines";
    }
    else if (cmd == "READ_FIRST") {
        std::string filename, extra;
        int n = 0;
        if (!(iss >> filename >> n) || (iss >> extra) || n <= 0 || n > 500) {
            return "ERR_BAD_ARGS: Usage: READ_FIRST <filename> <n (1..500)>";
        }
        fs::path target;
        if (!resolveSandboxPath(sandboxRoot, filename, target)) {
            return "ERR_ACCESS_DENIED: Path escapes sandbox or invalid";
        }
        if (!fs::exists(target) || !fs::is_regular_file(target)) {
            return "ERR_FILE_NOT_FOUND: File does not exist";
        }
        std::ifstream fin(target);
        if (!fin.is_open()) return "ERR_IO: Cannot open file";
        std::ostringstream out;
        std::string line;
        int count = 0;
        while (count < n && std::getline(fin, line)) {
            if (count > 0) out << "\n";
            out << line;
            ++count;
        }
        isOk = true;
        std::string text = escapePayload(out.str());
        if (text.size() > 60000) text = text.substr(0, 60000) + "...[TRUNCATED]";
        return text.empty() ? "(empty file)" : text;
    }
    else if (cmd == "QUIT") {
        isOk = true;
        return "BYE";
    }
    return "ERR_UNKNOWN_CMD: Unknown command '" + cmd + "'";
}

void handleClient(SOCKET clientSock, std::string peerAddr, fs::path sandboxRoot, int timeoutMs) {
    DWORD rcvTimeout = static_cast<DWORD>(timeoutMs);
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

    std::string clientId = "unknown";
    Logger::log("SERVER", clientId, peerAddr, "CLIENT_CONNECTED", "-", "OK");

    while (g_running) {
        std::string frame;
        int status = netproto::recvFrame(clientSock, frame);
        if (status == 0) {
            Logger::log("SERVER", clientId, peerAddr, "CLIENT_DISCONNECTED", "-", "CLOSED_BY_PEER");
            break;
        }
        if (status == -2) {
            Logger::log("SERVER", clientId, peerAddr, "RECV_ERROR", "-", "INVALID_FRAME_SIZE");
            netproto::sendFrame(clientSock, "RES|1.0|0|ERR|ERR_PROTOCOL: Frame size out of bounds");
            break;
        }
        if (status < 0) {
            int err = WSAGetLastError();
            std::string reason = (err == WSAETIMEDOUT) ? "TIMEOUT" : ("WSAERR_" + std::to_string(err));
            Logger::log("SERVER", clientId, peerAddr, "RECV_ERROR", "-", reason);
            break;
        }

        // Формат запроса: REQ|version|clientId|requestId|commandText
        size_t p1 = frame.find('|');
        size_t p2 = (p1 != std::string::npos) ? frame.find('|', p1 + 1) : std::string::npos;
        size_t p3 = (p2 != std::string::npos) ? frame.find('|', p2 + 1) : std::string::npos;
        size_t p4 = (p3 != std::string::npos) ? frame.find('|', p3 + 1) : std::string::npos;

        if (p4 == std::string::npos || frame.substr(0, p1) != "REQ") {
            Logger::log("SERVER", clientId, peerAddr, "PARSE_ERROR", "-", "MALFORMED_REQUEST");
            netproto::sendFrame(clientSock, "RES|1.0|0|ERR|ERR_PROTOCOL: Expected REQ|ver|cid|reqId|cmd");
            continue;
        }

        std::string ver = frame.substr(p1 + 1, p2 - p1 - 1);
        clientId = frame.substr(p2 + 1, p3 - p2 - 1);
        std::string reqId = frame.substr(p3 + 1, p4 - p3 - 1);
        std::string cmdText = frame.substr(p4 + 1);

        if (ver != netproto::PROTOCOL_VERSION) {
            Logger::log("SERVER", clientId, peerAddr, "VERSION_MISMATCH", reqId, ver);
            netproto::sendFrame(clientSock, "RES|1.0|" + reqId + "|ERR|ERR_VERSION: Unsupported version");
            continue;
        }

        Logger::log("SERVER", clientId, peerAddr, "RECV_CMD", reqId, cmdText);

        bool isOk = false;
        std::string resultData = executeCommand(sandboxRoot, cmdText, isOk);
        std::string response = std::string("RES|1.0|") + reqId + "|" + (isOk ? "OK" : "ERR") + "|" + resultData;

        if (!netproto::sendFrame(clientSock, response)) {
            Logger::log("SERVER", clientId, peerAddr, "SEND_ERROR", reqId, "FAILED");
            break;
        }
        Logger::log("SERVER", clientId, peerAddr, "SEND_RES", reqId, isOk ? "OK" : "ERR");

        if (isOk && resultData == "BYE") {
            Logger::log("SERVER", clientId, peerAddr, "CLIENT_QUIT", reqId, "BYE");
            break;
        }
    }

    shutdown(clientSock, SD_BOTH);
    closesocket(clientSock);
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 20001;
    int timeoutMs = 30000;
    std::string sandboxDir = "sandbox";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--timeout" && i + 1 < argc) timeoutMs = std::stoi(argv[++i]);
        else if (arg == "--sandbox" && i + 1 < argc) sandboxDir = argv[++i];
    }

    std::signal(SIGINT, signalHandler);
    Logger::init("server.log");

    fs::path sandboxPath(sandboxDir);
    if (!fs::exists(sandboxPath)) {
        fs::create_directories(sandboxPath);
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        netproto::printWsaError("WSAStartup");
        return 1;
    }

    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET) {
        netproto::printWsaError("socket");
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid host IP address: " << host << "\n";
        closesocket(g_listenSock);
        WSACleanup();
        return 1;
    }

    if (bind(g_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        netproto::printWsaError("bind");
        closesocket(g_listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(g_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        netproto::printWsaError("listen");
        closesocket(g_listenSock);
        WSACleanup();
        return 1;
    }

    Logger::log("SERVER", "0", host + ":" + std::to_string(port), "START", "-", "SANDBOX=" + fs::weakly_canonical(sandboxPath).string());

    while (g_running) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(g_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCKET) {
            if (g_running) netproto::printWsaError("accept");
            break;
        }
        char ipStr[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string peer = std::string(ipStr) + ":" + std::to_string(ntohs(clientAddr.sin_port));

        std::thread(handleClient, clientSock, peer, sandboxPath, timeoutMs).detach();
    }

    Logger::log("SERVER", "0", "-", "SHUTDOWN", "-", "OK");
    if (g_listenSock != INVALID_SOCKET) closesocket(g_listenSock);
    WSACleanup();
    return 0;
}