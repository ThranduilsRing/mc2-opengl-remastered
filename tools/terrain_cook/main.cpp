#include "terrain_surface_cook.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::vector<uint8_t> read(const char* path, size_t expected) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != std::streamoff(expected)) throw std::runtime_error("input size mismatch");
    std::vector<uint8_t> bytes(expected);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(expected))) throw std::runtime_error("input read failed");
    return bytes;
}
uint32_t side(const char* text) {
    size_t end = 0;
    const auto n = std::stoul(text, &end);
    if (text[end] || n == 0 || n > mc2terrain::SurfaceCookInput::kMaxSourceSide) throw std::runtime_error("invalid image side");
    return static_cast<uint32_t>(n);
}
float scalar(const char* text) {
    size_t end = 0;
    const auto n = std::stof(text, &end);
    if (text[end]) throw std::runtime_error("invalid scalar");
    return n;
}
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "terrain_cook source.rgba width height world_span output.tsf [--srgb] [--origin x y] "
                     "[--shore grid.f32 width height water] [--shore-bounds x y width height] [--breakup]\n";
        return 2;
    }
    try {
        mc2terrain::SurfaceCookInput in;
        in.colorWidth = side(argv[2]); in.colorHeight = side(argv[3]);
        in.worldSpan = scalar(argv[4]); in.topLeftY = in.worldSpan;
        in.outputWidth = std::min(in.colorWidth * 2, mc2terrain::SurfaceAsset::kMaxSide);
        in.outputHeight = std::min(in.colorHeight * 2, mc2terrain::SurfaceAsset::kMaxSide);
        bool customShoreBounds = false;
        std::vector<float> shore;
        for (int i = 6; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--srgb") in.srgb = true;
            else if (option == "--breakup") in.staticBreakup = true;
            else if (option == "--origin" && i + 2 < argc) {
                in.topLeftX = scalar(argv[++i]); in.topLeftY = scalar(argv[++i]);
            } else if (option == "--shore-bounds" && i + 4 < argc) {
                for (float& value : in.shoreBounds) value = scalar(argv[++i]);
                customShoreBounds = true;
            }
            else if (option == "--shore" && i + 4 < argc) {
                const char* path = argv[++i];
                in.shoreWidth = side(argv[++i]); in.shoreHeight = side(argv[++i]);
                in.waterElevation = scalar(argv[++i]);
                auto bytes = read(path, size_t(in.shoreWidth) * in.shoreHeight * sizeof(float));
                shore.resize(bytes.size() / sizeof(float));
                // Authoring f32 input is IEEE little-endian, as on the supported Windows host.
                std::memcpy(shore.data(), bytes.data(), bytes.size());
                in.shore = shore;
                in.followCoast = true;
            } else throw std::runtime_error("unknown or incomplete option");
        }
        if (!customShoreBounds) in.shoreBounds = {in.topLeftX, in.topLeftY, in.worldSpan, in.worldSpan};
        auto rgba = read(argv[1], size_t(in.colorWidth) * in.colorHeight * 4);
        in.rgba = rgba;
        auto asset = mc2terrain::cookSurface(in);
        if (!asset) throw std::runtime_error("invalid cook inputs");
        auto bytes = mc2terrain::encodeSurfaceAsset(*asset);
        std::ofstream file(argv[5], std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())))
            throw std::runtime_error("output write failed");
        file.close();
        if (!file) throw std::runtime_error("output close failed");
        std::cout << "TSF1 " << asset->width << 'x' << asset->height << " source=" << asset->sourceHash << " bytes=" << bytes.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain_cook: " << error.what() << '\n';
        return 1;
    }
}
