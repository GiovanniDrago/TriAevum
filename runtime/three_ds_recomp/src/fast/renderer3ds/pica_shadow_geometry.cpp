#include "fast/renderer3ds/pica_shadow_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Fast::Renderer3ds {
namespace {
using Vec4 = std::array<float, 4>;
ShadowMatrix Identity() noexcept {
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}
ShadowMatrix Multiply(const ShadowMatrix& a, const ShadowMatrix& b) noexcept {
    ShadowMatrix result{};
    for (size_t row = 0; row < 4; ++row)
        for (size_t col = 0; col < 4; ++col) {
            double sum = 0;
            for (size_t k = 0; k < 4; ++k)
                sum += double(a[k * 4 + row]) * b[col * 4 + k];
            result[col * 4 + row] = static_cast<float>(sum);
        }
    return result;
}
ShadowVector Transform(const ShadowMatrix& matrix, const Vec4& point) noexcept {
    ShadowVector result{};
    for (size_t row = 0; row < 3; ++row) {
        double sum = 0;
        for (size_t col = 0; col < 4; ++col)
            sum += double(matrix[col * 4 + row]) * point[col];
        result[row] = static_cast<float>(sum);
    }
    return result;
}
bool UniformMatrix(std::span<const uint8_t> bytes, uint32_t first, ShadowMatrix& result) noexcept {
    if (first > 93 || bytes.size() < (first + 3) * sizeof(Vec4))
        return false;
    result = Identity();
    for (size_t row = 0; row < 3; ++row) {
        Vec4 values;
        std::memcpy(values.data(), bytes.data() + (first + row) * sizeof(Vec4), sizeof(values));
        for (size_t col = 0; col < 4; ++col) {
            if (!std::isfinite(values[col]))
                return false;
            result[col * 4 + row] = values[col];
        }
    }
    return true;
}
float Scalar(std::span<const uint8_t> bytes, PicaVertexUniformComponent component) noexcept {
    if (!component.Available() || bytes.size() < (component.Uniform + 1U) * sizeof(Vec4))
        return std::numeric_limits<float>::quiet_NaN();
    float value;
    std::memcpy(&value, bytes.data() + component.Uniform * sizeof(Vec4) + component.Component * sizeof(float),
                sizeof(value));
    return value;
}
struct AttributeStream {
    std::span<const uint8_t> Bytes;
    uint32_t Stride = 0;
    bool PerInstance = false;

    bool Read(uint32_t vertex, Vec4& result) const noexcept {
        const uint64_t offset = PerInstance ? 0 : uint64_t(vertex) * Stride;
        if (Stride == 0 || offset > Bytes.size() || sizeof(result) > Bytes.size() - offset)
            return false;
        std::memcpy(result.data(), Bytes.data() + offset, sizeof(result));
        return std::all_of(result.begin(), result.end(), [](float v) { return std::isfinite(v); });
    }
};

AttributeStream ResolveAttribute(const PicaShadowGeometryView& geometry, uint8_t location) noexcept {
    const auto attribute = std::find_if(geometry.Attributes.begin(), geometry.Attributes.end(),
                                        [location](const auto& a) { return a.Location == location; });
    if (attribute == geometry.Attributes.end())
        return {};
    const auto binding = std::find_if(geometry.Bindings.begin(), geometry.Bindings.end(),
                                      [&](const auto& b) { return b.Binding == attribute->Binding; });
    if (binding == geometry.Bindings.end() || binding->Offset < geometry.BufferBaseOffset || binding->Stride == 0 ||
        attribute->ByteOffset > binding->Size)
        return {};
    const auto input = std::find_if(geometry.InputBindings.begin(), geometry.InputBindings.end(),
                                    [&](const auto& b) { return b.Binding == attribute->Binding; });
    const uint64_t base = binding->Offset - geometry.BufferBaseOffset;
    if (input == geometry.InputBindings.end() || base > geometry.Payload.size() ||
        attribute->ByteOffset > geometry.Payload.size() - base)
        return {};
    const uint64_t offset = base + attribute->ByteOffset;
    const uint64_t size = std::min<uint64_t>(binding->Size - attribute->ByteOffset, geometry.Payload.size() - offset);
    return { geometry.Payload.subspan(offset, size), binding->Stride, input->PerInstance };
}
uint64_t Hash(uint64_t hash, uint64_t value) noexcept {
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        hash = (hash ^ uint8_t(value)) * 1099511628211ULL;
        value >>= 8;
    }
    return hash;
}
} // namespace

VirtualShadowCaster BuildPicaShadowCaster(const PicaShadowGeometryView& geometry,
                                          const PicaVertexShaderHookLayout& hooks, uint32_t booleanUniforms,
                                          std::span<const uint8_t> floatUniforms, const ShadowMatrix& viewToWorld,
                                          uint64_t geometryVersion, PicaShadowCasterCache& cache) noexcept {
    const auto* transform = hooks.TransformProgram();
    if (!transform || geometry.VertexOrIndexCount == 0)
        return {};
    ShadowMatrix postView, model;
    if (!UniformMatrix(floatUniforms, transform->ViewFirstUniform, postView) ||
        !UniformMatrix(floatUniforms, transform->ModelFirstUniform, model))
        return {};
    const auto worldFromModel = Multiply(viewToWorld, postView);
    const auto* skeleton = hooks.SkeletonProgram();
    const bool skinned = skeleton && (booleanUniforms & (1U << skeleton->EnableBooleanUniform)) != 0;
    const auto* surface = hooks.SurfaceProgram();
    if (skinned && (!surface || !surface->SkinnedNormalsAvailable() || !surface->SkinnedPositionScale.Available()))
        return {};
    uint64_t signature = 0;
    if (!skinned && geometryVersion != 0) {
        const auto world = Multiply(worldFromModel, model);
        signature = Hash(1469598103934665603ULL, geometryVersion);
        for (size_t index = 0; index < world.size(); ++index) {
            // Undoing float32 view matrices introduces sub-texel roundoff.
            // The bounds guard below contains the entire quantization cell.
            const double scale = index >= 12 ? 64.0 : 65536.0;
            const double value = double(world[index]) * scale;
            if (!std::isfinite(value) || std::abs(value) > 9.0e15)
                return {};
            signature = Hash(signature, static_cast<uint64_t>(static_cast<int64_t>(std::llround(value))));
        }
        if (signature == 0)
            signature = 1;
        if (cache.Signature == signature && cache.Bounds.Available)
            return { cache.Bounds, signature };
    }
    const bool multiple = skinned && skeleton->MultipleInfluenceBooleanUniform < 16 &&
                          (booleanUniforms & (1U << skeleton->MultipleInfluenceBooleanUniform)) != 0;
    uint8_t influences = 1;
    std::array<bool, 4> influenceEnabled{ true, false, false, false };
    float positionScale = 1, indexScale = 1, weightScale = 1;
    if (skinned) {
        positionScale = Scalar(floatUniforms, surface->SkinnedPositionScale);
        indexScale = Scalar(floatUniforms, surface->BoneIndexScale);
        weightScale = Scalar(floatUniforms, surface->BoneWeightScale);
        if (!std::isfinite(positionScale) || !std::isfinite(indexScale) || !std::isfinite(weightScale))
            return {};
        if (multiple) {
            influences = std::min<uint8_t>(4, skeleton->MaximumInfluences);
            influenceEnabled[1] = true;
            const float count = Scalar(floatUniforms, surface->InfluenceCount);
            if (!std::isfinite(count))
                return {};
            for (size_t i = 2; i < influences; ++i) {
                const float threshold = Scalar(floatUniforms, surface->AdditionalInfluenceThresholds[i - 2]);
                if (!std::isfinite(threshold))
                    return {};
                influenceEnabled[i] = threshold <= count;
            }
        }
        influences = std::min(influences, skeleton->MaximumInfluences);
    }
    ShadowBounds bounds;
    bool homogeneousRigid = true;
    float maximumLocalL1 = 0;
    const auto positions = ResolveAttribute(geometry, transform->PositionInputRegister);
    const auto boneIndices = skinned ? ResolveAttribute(geometry, skeleton->BoneIndexInputRegister) : AttributeStream{};
    const auto boneWeights =
        multiple ? ResolveAttribute(geometry, skeleton->BoneWeightInputRegister) : AttributeStream{};
    std::array<ShadowMatrix, 96> worldBones;
    std::array<bool, 96> boneReady{};
    auto worldLinear = worldFromModel;
    worldLinear[12] = worldLinear[13] = worldLinear[14] = 0;
    for (uint32_t item = 0; item < geometry.VertexOrIndexCount; ++item) {
        uint32_t rawIndex = item;
        if (geometry.Indexed) {
            const uint64_t offset = geometry.IndexOffset + uint64_t(item) * sizeof(uint16_t);
            if (offset > geometry.Payload.size() || sizeof(uint16_t) > geometry.Payload.size() - offset)
                return {};
            uint16_t index;
            std::memcpy(&index, geometry.Payload.data() + offset, sizeof(index));
            rawIndex = index;
        }
        const int64_t adjusted = int64_t(rawIndex) + geometry.BaseVertex;
        if (adjusted < 0 || adjusted > std::numeric_limits<uint32_t>::max())
            return {};
        const auto vertex = static_cast<uint32_t>(adjusted);
        Vec4 position;
        if (!positions.Read(vertex, position))
            return {};
        maximumLocalL1 =
            std::max(maximumLocalL1, std::abs(position[0]) + std::abs(position[1]) + std::abs(position[2]));
        ShadowVector modelPosition{};
        if (skinned) {
            Vec4 indices, weights{};
            if (!boneIndices.Read(vertex, indices) || (multiple && !boneWeights.Read(vertex, weights)))
                return {};
            for (auto& component : position)
                component *= positionScale;
            for (uint8_t influence = 0; influence < influences; ++influence) {
                if (!influenceEnabled[influence])
                    continue;
                const float offset = indices[influence] * indexScale;
                if (!std::isfinite(offset) || offset < 0 || offset > 95)
                    return {};
                const auto boneIndex = static_cast<uint32_t>(offset);
                if (!boneReady[boneIndex]) {
                    ShadowMatrix bone;
                    if (!UniformMatrix(floatUniforms, skeleton->PaletteFirstUniform + boneIndex, bone))
                        return {};
                    // Resolve each palette matrix once, not once per indexed
                    // corner/influence. The post-skin translation is added
                    // separately because native skinning restores W to one.
                    worldBones[boneIndex] = Multiply(worldLinear, bone);
                    boneReady[boneIndex] = true;
                }
                const auto transformed = Transform(worldBones[boneIndex], position);
                const float weight = multiple ? weights[influence] * weightScale : 1;
                for (size_t axis = 0; axis < 3; ++axis)
                    modelPosition[axis] += transformed[axis] * weight;
            }
        } else {
            homogeneousRigid &= position[3] == 1.0F;
            modelPosition = Transform(model, position);
        }
        const auto world =
            skinned ? ShadowVector{ modelPosition[0] + worldFromModel[12], modelPosition[1] + worldFromModel[13],
                                    modelPosition[2] + worldFromModel[14] }
                    : Transform(worldFromModel, { modelPosition[0], modelPosition[1], modelPosition[2], 1 });
        if (!std::all_of(world.begin(), world.end(), [](float v) { return std::isfinite(v); }))
            return {};
        if (!bounds.Available) {
            bounds.Minimum = bounds.Maximum = world;
            bounds.Available = true;
        } else
            for (size_t axis = 0; axis < 3; ++axis) {
                bounds.Minimum[axis] = std::min(bounds.Minimum[axis], world[axis]);
                bounds.Maximum[axis] = std::max(bounds.Maximum[axis], world[axis]);
            }
    }
    if (!bounds.Available)
        return {};
    for (size_t axis = 0; axis < 3; ++axis) {
        // Conservative against matrix cancellation and cached rigid pose bins.
        const float guard = 0.05F + maximumLocalL1 / 65536.0F +
                            std::max(std::abs(bounds.Minimum[axis]), std::abs(bounds.Maximum[axis])) * 2.0e-5F;
        bounds.Minimum[axis] -= guard;
        bounds.Maximum[axis] += guard;
    }
    if (!homogeneousRigid)
        signature = 0;
    if (signature != 0)
        cache = { signature, bounds };
    return { bounds, signature };
}

} // namespace Fast::Renderer3ds
