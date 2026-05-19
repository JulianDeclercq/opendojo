#include <windows.h>  // first — keeps Windows SDK happy regardless of order in callers

#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <share.h>  // _SH_DENYNO so the log can be tailed live in another process
#include <string>

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

// Drop the log next to the exe. For Tekken that's
// <TEKKEN 8>\Polaris\Binaries\Win64\opendojo.log.
std::filesystem::path log_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"opendojo.log";
    return std::filesystem::path(buf).parent_path() / L"opendojo.log";
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

bool opendojo::log::init() {
    std::lock_guard guard(g_mutex);
    if (g_file) return true;
    auto path = log_path();
    // _wfsopen with _SH_DENYNO leaves the file readable & writable by
    // other processes while we're holding it open — without it the user
    // can't tail or even open the log while the game is running.
    g_file = _wfsopen(path.wstring().c_str(), L"w", _SH_DENYNO);
    if (!g_file) return false;
    write_timestamp(g_file);
    std::fprintf(g_file, "OpenDojo log opened: %ls\n", path.wstring().c_str());
    std::fflush(g_file);
    return true;
}

void opendojo::log::shutdown() {
    std::lock_guard guard(g_mutex);
    if (g_file) {
        write_timestamp(g_file);
        std::fprintf(g_file, "OpenDojo log closed\n");
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void opendojo::log::write(std::string_view line) {
    std::lock_guard guard(g_mutex);
    if (!g_file) return;
    write_timestamp(g_file);
    std::fprintf(g_file, "%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(g_file);
}

void opendojo::log::format(const char* fmt, ...) {
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
