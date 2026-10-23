#pragma once

#include <string>

namespace sp::log {

void Init(const std::wstring& path);
void Write(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace sp::log

#define LOG(...) ::sp::log::Write(__VA_ARGS__)
