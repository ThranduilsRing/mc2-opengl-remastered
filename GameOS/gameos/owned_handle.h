// owned_handle.h -- a move-only owner of one handle whose release needs a context (a device, an allocator).
//
// WHAT: the destructor is the only place the handle is released; there is no `release()` that gives it up and no copy, so a handle has exactly one owner and
// is released exactly once, on every path out of a scope (an early `return false` in an init function included).
// WHY: the Vulkan islands used to pair every `vkCreate*` with a hand-written `vkDestroy*` in a teardown function, and a partial init or a forgotten line
// leaked a device object that only the validation layer saw at device destroy. The template has no Vulkan include so the semantics (move, reset, exactly-once
// release) are unit-tested without a Vulkan link.
#ifndef MC2_OWNED_HANDLE_H
#define MC2_OWNED_HANDLE_H

#include <utility>

namespace mc2 {

// `Release` is a stateless functor `void(Context, Handle)`. A value-initialised Handle is the empty state (nullptr for the Vulkan handle types).
template <class Handle, class Context, class Release>
class OwnedHandle {
public:
    using HandleType = Handle;

    OwnedHandle() = default;
    OwnedHandle(Context context, Handle handle) noexcept : context_(context), handle_(handle) {}
    ~OwnedHandle() { reset(); }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept : context_(other.context_), handle_(std::exchange(other.handle_, Handle{})) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            context_ = other.context_;
            handle_ = std::exchange(other.handle_, Handle{});
        }
        return *this;
    }

    Handle get() const noexcept { return handle_; }
    Context context() const noexcept { return context_; }
    explicit operator bool() const noexcept { return handle_ != Handle{}; }

    // Releases the held handle now (a no-op when empty).
    void reset() noexcept {
        if (handle_ != Handle{}) {
            Release{}(context_, handle_);
            handle_ = Handle{};
        }
    }

private:
    Context context_{};
    Handle handle_{};
};

}  // namespace mc2

#endif  // MC2_OWNED_HANDLE_H
