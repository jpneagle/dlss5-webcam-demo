#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>

// Device and file names can hold non-ASCII text; the log file is UTF-8.
inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

class Log {
public:
    static Log& Get() { static Log l; return l; }

    void Write(const std::string& s) {
        std::lock_guard<std::mutex> lock(m_mutex);
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream line;
        line << '[' << std::setfill('0') << std::setw(2) << st.wHour << ':'
             << std::setw(2) << st.wMinute << ':' << std::setw(2) << st.wSecond
             << '.' << std::setw(3) << st.wMilliseconds << "] " << s << "\n";
        OutputDebugStringA(line.str().c_str());
        m_file << line.str();
        m_file.flush();
    }
private:
    // Beside the executable, so the log sits next to ngx_logs/ no matter which
    // directory the demo was launched from.
    static std::filesystem::path LogPath() {
        wchar_t exe[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!n || n >= MAX_PATH) return std::filesystem::path(L"DLSSCamDemo.log");
        return std::filesystem::path(exe).parent_path() / L"DLSSCamDemo.log";
    }
    Log() : m_file(LogPath(), std::ios::out | std::ios::trunc) {}
    std::ofstream m_file;
    std::mutex m_mutex;
};

#define LOG(x) do { std::ostringstream _oss; _oss << x; Log::Get().Write(_oss.str()); } while(0)
