#include "../code/tick_commit.h"
#include "../RenderCore/TextureService.h"
#include "sim_overlap.h"
// Session source indices and reference lifetime; no file or device dependencies.
#include "texture_sources_internal.h"
#include "texture_ref.h"
#include "mc2fmt.h"
#include <limits>
#include <unordered_set>

namespace mc2::textures {
namespace { using Bytes=std::vector<uint8_t>; constexpr DWORD kInvalidAsset=0xffffffffu; }
TextureSources* sourcesIfReady() {
    return RenderCore::activeTextureService
        ? static_cast<TextureSources*>(RenderCore::textureService().sourceOwner()) : nullptr;
}
std::shared_ptr<TextureSource> liveSource(DWORD asset) {
    auto* store = sourcesIfReady();
    auto source = store ? store->get(asset) : nullptr;
    return source && RenderCore::textureService().isLive(source->handle) ? source : nullptr;
}
void queueCpuTexture(TextureSource& source) {
    if (!source.cpuTexture || !source.retired) return;
    std::lock_guard lock(source.retired->mutex);
    source.retired->pending.push_back(source.cpuTexture);
    source.cpuTexture=0;
}
TextureSource::~TextureSource() { queueCpuTexture(*this); }


DWORD assetCapacity() { auto* store=sourcesIfReady(); return store ? store->capacity() : 1; }
void flushSources(bool all) { if (auto* store=sourcesIfReady()) store->flush(all); }
void TextureSources::collectRetired() {
    mc2::overlap::assertMainThread("TextureSources::collectRetired");
    std::vector<DWORD> retired;
    { std::lock_guard lock(retired_->mutex); retired.swap(retired_->pending); }
    for (auto texture : retired)
        if (retireCpu_) retireCpu_(texture);
}
std::string TextureSources::nameKey(const char* name, DWORD variant) {
    std::string key=name ? name : "";
    for (char& c : key) {
        if (c=='\\') c='/';
        else if (c>='A' && c<='Z') c=char(c-'A'+'a');
    }
    return key+'|'+std::to_string(variant);
}
std::shared_ptr<TextureSource> TextureSources::find(const char* name, DWORD variant) {
    auto it=named_.find(nameKey(name,variant));
    if (it==named_.end()) return {};
    auto source=get(it->second);
    if (source && RenderCore::textureService().isLive(source->handle)) return source;
    named_.erase(it); return {};
}
std::shared_ptr<const Bytes> TextureSources::internBytes(const std::string& identity, std::shared_ptr<const Bytes> bytes) {
    auto& slot=bytes_[identity];
    if (auto existing=slot.lock()) return existing;
    slot=bytes; return bytes;
}
DWORD TextureSources::publish(std::shared_ptr<TextureSource> source) {
    std::lock_guard lock(indexMutex_);
    if (assets_.size() >= kTextureAssetLimit) return kInvalidAsset;
    const DWORD id=static_cast<DWORD>(assets_.size());
    source->asset=id; source->retired=retired_;
    if (source->key.source.empty())
        source->key.source=RenderCore::makeTextureSourceId("runtime",std::to_string(id),
            RenderCore::textureContentHash(source->bytes->data(),source->bytes->size()));
    source->key.format=source->format; source->key.hints=source->hints; source->key.variant=source->variant;
    RenderCore::TextureDimensions dims;
    if (source->width && source->height) { dims.width=source->width; dims.height=source->height; }
    if (source->logicalWidth && source->logicalHeight) { dims.logicalWidth=source->logicalWidth; dims.logicalHeight=source->logicalHeight; }
    auto& service=RenderCore::textureService();
    source->handle=service.acquire(source->key,id,dims);
    if (!source->handle.isValid()) return kInvalidAsset;
    service.attachSource(source->handle,source);
    assets_.push_back(source);
    if (!source->name.empty()) named_[nameKey(source->name.c_str(),source->variant)]=id;
    return id;
}
bool TextureSources::retain(const std::shared_ptr<TextureSource>& source) {
    if (source->users==(std::numeric_limits<DWORD>::max)() ||
        !RenderCore::textureService().retainSource(source->handle,source->asset)) return false;
    ++source->users; return true;
}
void TextureSources::release(const std::shared_ptr<TextureSource>& source) {
    if (!source->users || source->users <= source->ownedReferences) return;
    --source->users;
    auto& service=RenderCore::textureService();
    service.dropRef(source->asset);
    if (!source->users) service.onNodeDestroyed(source->asset);
}
void TextureSources::flush(bool all) {
    uint64_t encoded=0, owned=0, roots=0; uint32_t live=0;
    // Membership only: retain buffer owners while counting physical storage.
    // This set is never traversed and cannot determine submission order.
    std::unordered_set<std::shared_ptr<const Bytes>> unique;
    for (const auto& weak : assets_) if (auto source=weak.lock()) {
        if (!RenderCore::textureService().isLive(source->handle)) continue;
        ++live; owned+=source->ownedReferences; roots+=source->users-source->ownedReferences;
        if (source->bytes && unique.insert(source->bytes).second) encoded+=source->bytes->size();
        if (!all && source->lifetime) continue;
        const DWORD drop=all ? source->users : source->users-source->ownedReferences;
        for (DWORD i=0;i<drop;++i) RenderCore::textureService().dropRef(source->asset);
        source->users-=drop;
        if (all) source->ownedReferences=0;
        if (!source->users) RenderCore::textureService().onNodeDestroyed(source->asset);
        if (all) while (source->pins) { --source->pins; RenderCore::textureService().release(source->handle); }
    }
    collectRetired();
    RenderCore::textureService().clearCookedCache();
    mc2::fmt::print_out("[TEXTURE_SOURCES] boundary={} live={} roots={} owned={} encoded_bytes={} legacy_cache_bytes=0\n",
                       all ? "shutdown" : "mission",live,roots,owned,encoded);
}
void adoptOwned(DWORD asset) noexcept {
    if (auto source=liveSource(asset)) if (source->ownedReferences < source->users) ++source->ownedReferences;
}
void disown(DWORD asset) noexcept {
    if (auto source=liveSource(asset)) if (source->ownedReferences) --source->ownedReferences;
}
void releaseOwned(DWORD asset) noexcept { disown(asset); release(asset); }

DWORD findAndRetain(const char* path, gos_TextureFormat, DWORD, DWORD variant, DWORD) {
    auto* ready=sourcesIfReady(); if (!ready) return 0;
    auto& store=*ready; auto source=store.find(path,variant);
    return source && store.retain(source) ? source->asset : 0;
}
DWORD retain(DWORD asset) {
    auto source=liveSource(asset);
    return source && sourcesIfReady()->retain(source) ? asset : kInvalidAsset;
}
void release(DWORD asset) {
    { auto source=liveSource(asset); if (source) sourcesIfReady()->release(source); }
    if (!mc2::tick::collecting() && !mc2::overlap::onSimWorker()) if (auto* store=sourcesIfReady()) store->collectRetired();
}
DWORD uvScale(DWORD asset) { auto source=liveSource(asset); return source ? source->uvScale : 1; }
bool logicalSize(DWORD asset, DWORD& width, DWORD& height) {
    auto source=liveSource(asset);
    width=source ? source->logicalWidth : 0; height=source ? source->logicalHeight : 0;
    return width && height;
}
const char* name(DWORD asset) { auto source=liveSource(asset); return source && !source->name.empty() ? source->name.c_str() : nullptr; }
bool isDecoded(DWORD asset) {
    if (asset==0 || asset==kInvalidAsset) return true;
    auto source=liveSource(asset); return source && source->cpuTexture!=0;
}
void setSessionLifetime(DWORD asset, DWORD value) { if (auto source=liveSource(asset)) source->lifetime=value; }
void pin(DWORD asset) {
    if (auto source=liveSource(asset))
        if (source->pins!=(std::numeric_limits<DWORD>::max)() && RenderCore::textureService().retain(source->handle)) ++source->pins;
}
void unpin(DWORD asset) {
    { auto source=liveSource(asset); if (source && source->pins) { --source->pins; RenderCore::textureService().release(source->handle); } }
    if (!mc2::tick::collecting() && !mc2::overlap::onSimWorker()) if (auto* store=sourcesIfReady()) store->collectRetired();
}
DWORD pinCount(DWORD asset) { auto source=liveSource(asset); return source ? source->pins : 0; }

} // namespace mc2::textures

void mc2::TextureSourceRelease::adopt(std::uint32_t asset) const noexcept { mc2::textures::adoptOwned(asset); }
void mc2::TextureSourceRelease::disown(std::uint32_t asset) const noexcept { mc2::textures::disown(asset); }
void mc2::TextureSourceRelease::operator()(std::uint32_t asset) const noexcept { mc2::textures::releaseOwned(asset); }
