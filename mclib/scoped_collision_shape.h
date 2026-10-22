// Keep terrain queries on stock geometry without replacing the render instance.
#pragma once
#include <memory>

namespace mc2 {
template<class Shape>
class ScopedCollisionShape {
public:
    ScopedCollisionShape(Shape*& instance, long& lod, bool& pbr, Shape* collision)
        : instance_(instance), lod_(lod), pbr_(pbr), render_(instance),
          renderLod_(lod), renderPbr_(pbr), active_(collision != nullptr) {
        if (active_) {
            instance_ = collision;
            lod_ = 0;
            pbr_ = false;
        }
    }
    ~ScopedCollisionShape() {
        if (active_) {
            {
                // Legacy queries may replace the temporary instance. Adopt its
                // final value at exit so replacement and cleanup have one owner.
                std::unique_ptr<Shape> collision(instance_);
                if (collision)
                    collision->ClearAnimation();
            }
            instance_ = render_;
            lod_ = renderLod_;
            pbr_ = renderPbr_;
        }
    }
    ScopedCollisionShape(const ScopedCollisionShape&) = delete;
    ScopedCollisionShape& operator=(const ScopedCollisionShape&) = delete;
    bool active() const { return active_; }
private:
    Shape*& instance_;
    long& lod_;
    bool& pbr_;
    Shape* render_;
    long renderLod_;
    bool renderPbr_;
    bool active_;
};
}
