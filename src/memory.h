#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pattern.h"

namespace sp::mem {

struct Range {
    uintptr_t begin = 0;
    uintptr_t end = 0;

    bool Contains(uintptr_t address, size_t size = 1) const {
        return address >= begin && address + size <= end && address + size >= address;
    }
};

// The module's mapped image, [base, base + SizeOfImage).
Range ModuleRange(HMODULE module);

// Committed, executable, readable regions inside `range`.
std::vector<Range> CodeRegions(const Range& range);

// First match in `regions` for which accept(address) is true, or 0.
template <typename Accept>
uintptr_t FindIf(const Pattern& pattern, const std::vector<Range>& regions, Accept&& accept) {
    for (const Range& r : regions) {
        const auto* data = reinterpret_cast<const uint8_t*>(r.begin);
        const size_t off = pattern.FindIf(data, r.end - r.begin, [&](size_t o) { return accept(r.begin + o); });
        if (off != Pattern::npos) return r.begin + off;
    }
    return 0;
}

bool IsReadable(uintptr_t address, size_t size);

// Writes through page protection (code or read-only data) and flushes the instruction cache.
bool Write(uintptr_t address, const void* data, size_t size);

// Read-write memory within rel32 reach of `address`, or nullptr.
void* AllocNear(uintptr_t address, size_t size);

}  // namespace sp::mem
