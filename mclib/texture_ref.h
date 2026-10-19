// WHAT: the owner of one reference to a texture node of the texture manager (MC_TextureManager), as a move-only value.
// WHY: a node reference was a bare DWORD released by one of two functions with different meanings; a holder that forgot, or released twice,
// leaked or freed a texture under another user. The owner releases exactly once, when it dies or is reset, through the manager's one
// release function; the manager keeps the reference count, so a node that two users loaded survives until the last one lets go.
#pragma once

#include <cstdint>
#include <utility>

namespace mc2
{
// The manager's "no node" value (MC_TextureNode index); a holder that owns nothing carries it.
inline constexpr std::uint32_t kNoTextureNode = 0xffffffffu;

// Releaser is a default-constructible function object taking the node index; the engine's is TextureSourceRelease, a test passes its own.
template <class Releaser>
class BasicTextureRef
{
public:
	BasicTextureRef () noexcept = default;

	// Adopts one reference that loadTexture / textureFromMemory already counted for the caller.
	explicit BasicTextureRef (std::uint32_t node) noexcept : node_(node) {}

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
	}

	// Gives the node up WITHOUT releasing it: for a holder whose reference is deliberately kept until the manager is destroyed.
	std::uint32_t release () noexcept { return std::exchange(node_, kNoTextureNode); }

	std::uint32_t get () const noexcept { return node_; }
	explicit operator bool () const noexcept { return node_ != kNoTextureNode; }

private:
	std::uint32_t node_ = kNoTextureNode;
};

// The engine releases through the texture source API; it tolerates teardown.
struct TextureSourceRelease
{
	void operator() (std::uint32_t node) const noexcept;
};

using TextureRef = BasicTextureRef<TextureSourceRelease>;
} // namespace mc2
