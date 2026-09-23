#pragma once

#include <string_view>

namespace Fast::Renderer3ds {

// GPU form of DecodePicaTextureRgba8 for the tiled eight-bit-per-texel formats
// (0-4). One invocation per texture pixel: it computes the Morton source pixel
// exactly as the CPU oracle does, reads the bytes from an R32_UINT image holding
// the native tiled bytes, decodes the format and writes one RGBA8 word.
//
// The output is a word buffer rather than a storage image because RGBA8 storage
// images need shaderStorageImageExtendedFormats: the caller copies the buffer
// into the sampled RGBA8 image with vkCmdCopyBufferToImage, which is base
// Vulkan. Word packing is little-endian byte order R, G, B, A.
//
// Format numbering is the texture one, which is not the copy's: 0 RGBA8
// (memory order A,B,G,R), 1 RGB8 (B,G,R), 2 RGBA5551, 3 RGB565, 4 RGBA4444.
//
// Push constants: Extent = (width, height, nativeStrideWords, pixelCount),
//                 Layout = (format, bytesPerPixel, 0, 0).
inline constexpr std::string_view PicaTextureDecodeComputeSource = R"glsl(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, r32ui) uniform readonly uimage2D NativeWords;
layout(set = 0, binding = 1) buffer Decoded {
    uint pixels[];
} decoded;
layout(push_constant) uniform Decode {
    uvec4 Extent;
    uvec4 Layout;
} decode;

uint nativeByte(uint byteIndex) {
    const uint wordsPerRow = decode.Extent.z;
    const uint word = byteIndex / 4u;
    const uint value =
        imageLoad(NativeWords, ivec2(int(word % wordsPerRow), int(word / wordsPerRow))).x;
    return (value >> ((byteIndex % 4u) * 8u)) & 0xFFu;
}

uint expand4(uint value) { return (value << 4u) | value; }
uint expand5(uint value) { return (value << 3u) | (value >> 2u); }
uint expand6(uint value) { return (value << 2u) | (value >> 4u); }

void main() {
    const uint pixel = gl_GlobalInvocationID.x;
    if (pixel >= decode.Extent.w) {
        return;
    }
    const uint width = decode.Extent.x;
    const uint height = decode.Extent.y;
    const uint format = decode.Layout.x;
    const uint bytesPerPixel = decode.Layout.y;
    const uint alignedWidth = max(8u, (width + 7u) & ~7u);
    const uint x = pixel % width;
    const uint y = pixel / width;
    const uint tileIndex = (y / 8u) * (alignedWidth / 8u) + x / 8u;
    const uint localX = x % 8u;
    const uint localY = y % 8u;
    const uint morton = ((localX & 1u) << 0u) | ((localY & 1u) << 1u) |
                        ((localX & 2u) << 1u) | ((localY & 2u) << 2u) |
                        ((localX & 4u) << 2u) | ((localY & 4u) << 3u);
    const uint byteOffset = (tileIndex * 64u + morton) * bytesPerPixel;
    uint red = 0u, green = 0u, blue = 0u, alpha = 0u;
    if (format == 0u) {
        alpha = nativeByte(byteOffset + 0u);
        blue = nativeByte(byteOffset + 1u);
        green = nativeByte(byteOffset + 2u);
        red = nativeByte(byteOffset + 3u);
    } else if (format == 1u) {
        blue = nativeByte(byteOffset + 0u);
        green = nativeByte(byteOffset + 1u);
        red = nativeByte(byteOffset + 2u);
        alpha = 255u;
    } else {
        const uint packed = nativeByte(byteOffset + 0u) |
                            (nativeByte(byteOffset + 1u) << 8u);
        if (format == 2u) {
            red = expand5((packed >> 11u) & 0x1Fu);
            green = expand5((packed >> 6u) & 0x1Fu);
            blue = expand5((packed >> 1u) & 0x1Fu);
            alpha = (packed & 1u) != 0u ? 255u : 0u;
        } else if (format == 3u) {
            red = expand5((packed >> 11u) & 0x1Fu);
            green = expand6((packed >> 5u) & 0x3Fu);
            blue = expand5(packed & 0x1Fu);
            alpha = 255u;
        } else {
            red = expand4((packed >> 12u) & 0x0Fu);
            green = expand4((packed >> 8u) & 0x0Fu);
            blue = expand4((packed >> 4u) & 0x0Fu);
            alpha = expand4(packed & 0x0Fu);
        }
    }
    decoded.pixels[pixel] = red | (green << 8u) | (blue << 16u) | (alpha << 24u);
}
)glsl";

} // namespace Fast::Renderer3ds
