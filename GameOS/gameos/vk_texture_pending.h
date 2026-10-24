#pragma once
#include <cstddef>
#include <utility>

namespace mc2vk {
// A refused upload keeps its decoded pixels, but never monopolizes a pump or retries forever.
template<class Pending, class Uploads, class Budget, class Live, class Full, class Upload, class Drop>
void pumpPendingTextureUploads(Pending& pending, Uploads& uploads, Pending& batch, Budget& budget,
                               Live live, Full full, Upload upload, Drop drop) {
    for (std::size_t remaining=pending.size(); remaining && !pending.empty(); --remaining) {
        auto& p=pending.front();
        if (!live(p)) { pending.pop_front(); continue; }
        const std::size_t bytes=p.image->pixels.size();
        if (full() || !budget.reserve(bytes)) break;
        typename Uploads::value_type next{};
        next.handle=p.handle; next.bytes=bytes; next.contentVersion=p.contentVersion;
        if (!upload(p,next)) {
            budget.complete(bytes);
            if (++p.attempts >= 3) { drop(p); pending.pop_front(); }
            else { auto retry=std::move(p); pending.pop_front(); pending.push_back(std::move(retry)); }
            continue;
        }
        uploads.push_back(std::move(next));
        batch.push_back(std::move(p));
        pending.pop_front();
    }
}
} // namespace mc2vk
