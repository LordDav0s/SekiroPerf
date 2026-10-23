#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace sp::log {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
FILE* g_file = nullptr;
ULONGLONG g_start = 0;

}  // namespace

void Init(const std::wstring& path) {
    g_start = GetTickCount64();
    g_file = _wfopen(path.c_str(), L"w");
}

void Write(const char* fmt, ...) {
    if (!g_file) return;

    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    const ULONGLONG ms = GetTickCount64() - g_start;
    AcquireSRWLockExclusive(&g_lock);
    fprintf(g_file, "[%5lu.%03lu] %s\n", static_cast<unsigned long>(ms / 1000),
            static_cast<unsigned long>(ms % 1000), message);
    fflush(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace sp::log
