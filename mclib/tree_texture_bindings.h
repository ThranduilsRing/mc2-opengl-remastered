#pragma once
#include <cstdint>
#include <vector>

// Owned by one appearance type; texture handles belong to that mission.
class TreeTextureBindings {
public:
    struct Binding {
        std::uint32_t handle = 0xffffffffu;
        bool present = false;
        bool alpha = false;
    };
    void clear() { bindings_.clear(); ready_ = false; }

    template<class Shape, class Resolve>
    void apply(Shape& shape, Resolve&& resolve) {
        if (!ready_) {
            for (long i = 0; i < shape.GetNumTextures(); ++i) {
                char name[1024];
                shape.GetTextureName(i, name, 256);
                const bool alpha = (name[0] == 'a' || name[0] == 'A') && name[1] == '_';
                bindings_.push_back(resolve(name, alpha));
            }
            ready_ = true;
        }
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            const auto& binding = bindings_[i];
            shape.SetTextureHandle(i, binding.handle);
            // Legacy missing-texture handling deliberately preserves alpha.
            if (binding.present) shape.SetTextureAlpha(i, binding.alpha);
        }
    }
private:
    std::vector<Binding> bindings_;
    bool ready_ = false;
};
