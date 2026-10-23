#include "memory.h"

#include <algorithm>
#include <cstring>

namespace sp::mem {
namespace {

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kExecutable = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

bool Usable(const MEMORY_BASIC_INFORMATION& mbi, DWORD required) {
    return mbi.State == MEM_COMMIT && (mbi.Protect & required) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

}  // namespace

Range ModuleRange(HMODULE module) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

std::vector<Range> CodeRegions(const Range& range) {
    std::vector<Range> out;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = range.begin;
    while (p < range.end && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        if (Usable(mbi, kExecutable)) {
            out.push_back({std::max(regionBegin, range.begin), std::min(regionEnd, range.end)});
        }
        p = regionEnd;
    }
    return out;
}

bool IsReadable(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return Usable(mbi, kReadable) && address + size <= regionEnd;
}

bool Write(uintptr_t address, const void* data, size_t size) {
    void* target = reinterpret_cast<void*>(address);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(target, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;

    // Keep execute permission on code pages: other threads may be running code on them.
    const DWORD writable = (mbi.Protect & kExecutable) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old;
    if (!VirtualProtect(target, size, writable, &old)) return false;
    std::memcpy(target, data, size);
    DWORD ignored;
    VirtualProtect(target, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return true;
}

void* AllocNear(uintptr_t address, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t granularity = si.dwAllocationGranularity;
    const auto minApp = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxApp = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    // Stay well inside +/-2 GB so any address in the allocation is reachable by a rel32.
    constexpr uintptr_t kReach = 0x7FF00000;
    const uintptr_t lo = address > minApp + kReach ? address - kReach : minApp;
    const uintptr_t hi = address < maxApp - kReach ? address + kReach : maxApp;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = lo;
    while (p < hi && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const uintptr_t candidate = (std::max(regionBegin, lo) + granularity - 1) / granularity * granularity;
            if (candidate + size <= regionEnd && candidate + size <= hi) {
                if (void* mem = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE)) {
                    return mem;
                }
            }
        }
        p = regionEnd;
    }
    return nullptr;
}

}  // namespace sp::mem
