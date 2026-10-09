#include "oot3d_native_static_emitters.h"

#include "recomp/a32_runtime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace Oot3dNativeGame {
namespace {

bool ParseCategory(const std::string& name, NativeStaticEmitterCategory& category) {
    if (name == "torch") category = NativeStaticEmitterCategory::Torch;
    else if (name == "fire-lava") category = NativeStaticEmitterCategory::FireLava;
    else if (name == "window-lamp") category = NativeStaticEmitterCategory::WindowLamp;
    else if (name == "light-shaft") category = NativeStaticEmitterCategory::LightShaft;
    else if (name == "magic-glow") category = NativeStaticEmitterCategory::MagicGlow;
    else return false;
    return true;
}

bool ReadTriple(const nlohmann::json& value, std::array<float, 3>& out) {
    if (!value.is_array() || value.size() != 3U) return false;
    for (size_t i = 0; i < 3U; ++i) {
        if (!value[i].is_number()) return false;
        out[i] = value[i].get<float>();
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

} // namespace

std::shared_ptr<const NativeStaticEmitterCatalog> NativeStaticEmitterCatalog::LoadFile(
    const std::filesystem::path& path, std::string* error) {
    const auto fail = [&](const std::string& message) -> std::shared_ptr<const NativeStaticEmitterCatalog> {
        if (error) *error = message;
        return nullptr;
    };
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("cannot open " + path.string());
    const auto document = nlohmann::json::parse(file, nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        document.value("schema", std::string()) != "triaevum.static-emitters.v1" ||
        !document.contains("scenes") || !document["scenes"].is_object())
        return fail(path.string() + " is not a triaevum.static-emitters.v1 catalog");
    auto catalog = std::make_shared<NativeStaticEmitterCatalog>();
    for (const auto& [name, scene] : document["scenes"].items()) {
        if (!scene.is_object() || !scene.contains("scene") || !scene["scene"].is_number_unsigned() ||
            !scene.contains("rooms") || !scene["rooms"].is_object())
            return fail("scene " + name + " is malformed");
        if (scene.value("masterQuest", false)) continue;
        const auto sceneId = scene["scene"].get<uint32_t>();
        if (sceneId > 0xFFFFU) return fail("scene " + name + " has an invalid id");
        for (const auto& [roomName, emitters] : scene["rooms"].items()) {
            char* end = nullptr;
            const long room = std::strtol(roomName.c_str(), &end, 10);
            if (end == roomName.c_str() || *end != '\0' || room < 0 || room > 127 || !emitters.is_array())
                return fail("scene " + name + " room " + roomName + " is malformed");
            auto& out = catalog->mRooms[{static_cast<uint16_t>(sceneId), static_cast<int>(room)}];
            for (const auto& e : emitters) {
                NativeStaticEmitter emitter;
                if (!e.is_object() || !ReadTriple(e.value("position", nlohmann::json()), emitter.Position) ||
                    !ReadTriple(e.value("colour", nlohmann::json()), emitter.Colour) ||
                    !ParseCategory(e.value("category", std::string()), emitter.Category) ||
                    !e.contains("radius") || !e["radius"].is_number() || !e.contains("intensity") ||
                    !e["intensity"].is_number())
                    return fail("scene " + name + " room " + roomName + " has a malformed emitter");
                emitter.Radius = e["radius"].get<float>();
                emitter.Gain = e["intensity"].get<float>();
                if (!(emitter.Radius > 0.0F) || !(emitter.Gain >= 0.0F) || emitter.Gain > 4.0F)
                    return fail("scene " + name + " room " + roomName + " has an emitter out of range");
                for (auto& channel : emitter.Colour) channel = std::clamp(channel, 0.0F, 1.0F);
                out.push_back(emitter);
                ++catalog->mCount;
            }
        }
    }
    return catalog;
}

std::shared_ptr<const NativeStaticEmitterCatalog> NativeStaticEmitterCatalog::FromEnvironment() {
    const char* path = std::getenv("TRIAEVUM_STATIC_EMITTERS");
    if (path == nullptr || *path == '\0') return nullptr;
    std::string error;
    auto catalog = LoadFile(path, &error);
    if (catalog)
        std::fprintf(stderr, "TRIAEVUM_STATIC_EMITTERS loaded=%zu path=%s\n", catalog->Count(), path);
    else
        std::fprintf(stderr, "TRIAEVUM_STATIC_EMITTERS rejected: %s\n", error.c_str());
    return catalog;
}

std::span<const NativeStaticEmitter> NativeStaticEmitterCatalog::Room(uint16_t scene, int room) const {
    const auto found = mRooms.find({scene, room});
    return found == mRooms.end() ? std::span<const NativeStaticEmitter>{} : std::span(found->second);
}

std::vector<int> NativeLoadedRooms(uint32_t playState, oot3d::recomp::a32::MemoryBus& memory) {
    std::vector<int> rooms;
    if (playState == 0U) return rooms;
    for (const auto& [number, commands] : {std::pair{0x4C30U, 0x4FFCU}, std::pair{0x500CU, 0x53D8U}}) {
        uint8_t raw = 0;
        uint32_t installed = 0;
        if (!memory.Read8(playState + number, &raw) || !memory.Read32(playState + commands, &installed)) continue;
        const int room = std::bit_cast<int8_t>(raw);
        if (room >= 0 && installed != 0U && std::find(rooms.begin(), rooms.end(), room) == rooms.end())
            rooms.push_back(room);
    }
    return rooms;
}

void CaptureStaticEmitters(const NativeStaticEmitterCatalog& catalog, uint16_t scene, std::span<const int> rooms,
                           std::vector<Fast::Renderer3ds::PicaScenePointLight>& lights) {
    for (const int room : rooms) {
        const auto emitters = catalog.Room(scene, room);
        for (size_t i = 0; i < emitters.size(); ++i) {
            const auto& e = emitters[i];
            Fast::Renderer3ds::PicaScenePointLight light;
            light.SourceIdentity = (uint64_t{0xA5U} << 56U) | (uint64_t{scene} << 40U) |
                                   (uint64_t(static_cast<uint32_t>(room) & 0xFFU) << 32U) | uint64_t(i);
            light.Group = 3U;
            light.PositionWorld = e.Position;
            light.Radius = e.Radius;
            for (size_t c = 0; c < 3U; ++c) light.Color[c] = std::clamp(e.Colour[c] * e.Gain, 0.0F, 1.0F);
            // Torches are small, strong flames worth a cube map; broad sources
            // light without one.
            light.CastsShadow = e.Category == NativeStaticEmitterCategory::Torch;
            lights.push_back(light);
        }
    }
}

} // namespace Oot3dNativeGame
