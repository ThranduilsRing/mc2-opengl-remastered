#pragma once

// Failed encyclopedia stubs must release their registry user before plain lookup.
template<class Load, class Release>
auto choosePreviewType(bool encExists, bool& selectedEnc, Load load, Release release)
{
    selectedEnc = encExists;
    auto* type = load(selectedEnc);
    if (selectedEnc && (!type || type->baseShapeLoadFailed)) {
        if (type) release(type);
        selectedEnc = false;
        type = load(false);
    }
    if (type && type->baseShapeLoadFailed) {
        release(type);
        type = nullptr;
    }
    return type;
}
