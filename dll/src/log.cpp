#include <windows.h>  // first — keeps Windows SDK happy regardless of order in callers

#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
FILE*      g_file = nullptr;

// Drop the log next to the exe. For Tekken that's
// <TEKKEN 8>\Polaris\Binaries\Win64\openlab.log.
std::filesystem::path log_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"openlab.log";
    return std::filesystem::path(buf).parent_path() / L"openlab.log";
}

void write_timestamp(FILE* f) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) > 0) {
        std::fprintf(f, "[%s] ", buf);
    }
}

}  // namespace

bool openlab::log::init() {
    std::lock_guard guard(g_mutex);
    if (g_file) return true;
    auto path = log_path();
    if (_wfopen_s(&g_file, path.wstring().c_str(), L"w") != 0) {
        g_file = nullptr;
        return false;
    }
    write_timestamp(g_file);
    std::fprintf(g_file, "OpenLab log opened: %ls\n", path.wstring().c_str());
    std::fflush(g_file);
    return true;
}

void openlab::log::shutdown() {
    std::lock_guard guard(g_mutex);
    if (g_file) {
        write_timestamp(g_file);
        std::fprintf(g_file, "OpenLab log closed\n");
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void openlab::log::write(std::string_view line) {
    std::lock_guard guard(g_mutex);
    if (!g_file) return;
    write_timestamp(g_file);
    std::fprintf(g_file, "%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(g_file);
}

void openlab::log::format(const char* fmt, ...) {
    std::lock_guard guard(g_mutex);
    if (!g_file) return;
    write_timestamp(g_file);
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}
