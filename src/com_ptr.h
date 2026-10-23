#pragma once

#include <unknwn.h>

namespace sp {

// Minimal owning COM pointer (avoids depending on WRL, which not every MinGW ships).
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : p_(p) {}
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }

    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // Releases the current pointer and returns the slot for an out-parameter.
    T** Put() {
        Reset();
        return &p_;
    }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    template <typename U>
    HRESULT As(ComPtr<U>& out) const {
        return p_ ? p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.Put())) : E_POINTER;
    }

private:
    T* p_ = nullptr;
};

}  // namespace sp
