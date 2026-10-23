#pragma once

// Portable (no Windows dependencies) so it can be unit tested on any host.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace sp {

// IDA-style byte signature, e.g. "C7 43 ?? ?? 4C 89 AB". "?" or "??" matches any byte.
class Pattern {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    explicit Pattern(std::string_view text) {
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == ' ') {
                ++i;
                continue;
            }
            if (text[i] == '?') {
                bytes_.push_back(0);
                mask_.push_back(false);
                i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
                continue;
            }
            const int hi = HexValue(text[i]);
            const int lo = i + 1 < text.size() ? HexValue(text[i + 1]) : -1;
            if (hi < 0 || lo < 0) {
                // Malformed signature: make it never match rather than match garbage.
                bytes_.clear();
                mask_.clear();
                return;
            }
            bytes_.push_back(static_cast<uint8_t>(hi << 4 | lo));
            mask_.push_back(true);
            i += 2;
        }
        for (size_t k = 0; k < mask_.size(); ++k) {
            if (mask_[k]) {
                anchor_ = k;
                break;
            }
        }
    }

    size_t size() const { return bytes_.size(); }

    bool MatchesAt(const uint8_t* p) const {
        for (size_t k = 0; k < bytes_.size(); ++k) {
            if (mask_[k] && p[k] != bytes_[k]) return false;
        }
        return true;
    }

    // Offset of the first match starting at or after `start`, or npos.
    size_t Find(const uint8_t* data, size_t len, size_t start = 0) const {
        const size_t n = bytes_.size();
        if (n == 0 || anchor_ == npos || len < n) return npos;
        const size_t last = len - n;
        size_t pos = start;
        while (pos <= last) {
            // Jump straight to the next occurrence of the first fixed byte.
            const void* hit = std::memchr(data + pos + anchor_, bytes_[anchor_], last - pos + 1);
            if (!hit) return npos;
            pos = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data) - anchor_;
            if (MatchesAt(data + pos)) return pos;
            ++pos;
        }
        return npos;
    }

    // Offset of the first match for which accept(offset) returns true, or npos.
    template <typename Accept>
    size_t FindIf(const uint8_t* data, size_t len, Accept&& accept) const {
        for (size_t off = Find(data, len); off != npos; off = Find(data, len, off + 1)) {
            if (accept(off)) return off;
        }
        return npos;
    }

private:
    static int HexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    std::vector<uint8_t> bytes_;
    std::vector<bool> mask_;  // true = byte must match
    size_t anchor_ = npos;    // index of the first fixed byte
};

}  // namespace sp
