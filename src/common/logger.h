#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

class Logger {
public:
    static void init(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx_);
        file_.open(filename, std::ios::app);
    }

    static void log(const std::string& role, const std::string& clientId,
                    const std::string& peerAddr, const std::string& op,
                    const std::string& reqId, const std::string& status) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string line = "[" + timestamp() + "] " + role +
                           " id=" + clientId +
                           " peer=" + peerAddr +
                           " op=" + op +
                           " reqId=" + reqId +
                           " status=" + status;
        std::cout << line << "\n";
        if (file_.is_open()) {
            file_ << line << "\n";
            file_.flush();
        }
    }

private:
    static std::string timestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &t);
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%H:%M:%S") << "."
            << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    static inline std::mutex mtx_;
    static inline std::ofstream file_;
};