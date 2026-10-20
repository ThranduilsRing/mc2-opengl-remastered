// Owns one counted source-asset reference. Explicit owners survive mission-root
// cleanup; moves transfer ownership and handout transfers the reference to the
// caller. The releaser keeps the session boundary out of this pure C++ value.
#pragma once

#include <cstdint>
#include <utility>
#include <type_traits>

namespace mc2
{
// A holder that owns no source carries this compatibility asset value.
inline constexpr std::uint32_t kNoTextureNode = 0xffffffffu;

// Releaser is a default-constructible function object taking the node index; the engine's is TextureSourceRelease, a test passes its own.
struct TextureSourceRelease;

template <class Releaser>
class BasicTextureRef
{
public:
	BasicTextureRef () noexcept = default;

	// Adopts one reference that source load / fromPixels already counted for the caller.
	explicit BasicTextureRef (std::uint32_t node) noexcept : node_(node) { adoptRef(node_); }

	BasicTextureRef (const BasicTextureRef&) = delete;
	BasicTextureRef& operator= (const BasicTextureRef&) = delete;

	BasicTextureRef (BasicTextureRef&& other) noexcept : node_(std::exchange(other.node_, kNoTextureNode)) {}

	BasicTextureRef& operator= (BasicTextureRef&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			node_ = std::exchange(other.node_, kNoTextureNode);
		}
		return *this;
	}

	~BasicTextureRef () { reset(); }

	// Releases what is owned, then adopts `node` (or nothing).
	void reset (std::uint32_t node = kNoTextureNode) noexcept
	{
		const std::uint32_t old = std::exchange(node_, node);
		if (old != kNoTextureNode)
			Releaser{}(old);
		adoptRef(node_);
	}

	// Hands the counted reference to the caller without releasing it.
	std::uint32_t release () noexcept {
		const auto node = std::exchange(node_, kNoTextureNode);
		disownRef(node);
		return node;
	}

	std::uint32_t get () const noexcept { return node_; }
	explicit operator bool () const noexcept { return node_ != kNoTextureNode; }

private:
	// A source releaser can distinguish an explicitly owned reference from a
	// mission root. Moves transfer the category; handout returns it to the caller.
	static void adoptRef(std::uint32_t node) noexcept {
		if constexpr (requires { Releaser{}.adopt(node); }) {
			if (node != kNoTextureNode) Releaser{}.adopt(node);
		}
	}
	static void disownRef(std::uint32_t node) noexcept {
		if constexpr (requires { Releaser{}.disown(node); }) {
			if (node != kNoTextureNode) Releaser{}.disown(node);
		}
	}
	std::uint32_t node_ = kNoTextureNode;
};

// The engine releases through the texture source API; it tolerates teardown.
struct TextureSourceRelease
{
	void adopt(std::uint32_t node) const noexcept;
	void disown(std::uint32_t node) const noexcept;
	void operator() (std::uint32_t node) const noexcept;
};

using TextureRef = BasicTextureRef<TextureSourceRelease>;
} // namespace mc2
