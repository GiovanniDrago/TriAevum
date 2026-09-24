#include "azahar_pbr_materials.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace Oot3d::Renderer {
namespace {

std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

std::optional<uint64_t> ParseHash(std::string_view text) {
    if (text.empty() || text.size() > 16U) return std::nullopt;
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

struct Sidecar {
    std::optional<uint64_t> Hash;
    int Role = -1;
    bool Json = false;
    bool LegacyNormal = false;
    bool InvalidName = false;
};

Sidecar Describe(const std::filesystem::path& path) {
    Sidecar result;
    const auto name = Lower(path.filename().string());
    if (name.ends_with(".material.json")) {
        result.Hash = ParseHash(std::string_view(name).substr(0, name.size() - 14U));
        result.Json = result.Hash.has_value();
        return result;
    }
    const auto stem = Lower(path.stem().string());
    if (stem.ends_with(".norm")) {
        result.LegacyNormal = true;
        result.Role = 0;
        result.Hash = ParseAzaharTextureFilenameHash(path.stem());
        return result;
    }
    constexpr std::array<std::string_view, 3> suffixes{"_n", "_orm", "_e"};
    for (size_t role = 0; role < suffixes.size(); ++role) {
        if (!stem.ends_with(suffixes[role])) continue;
        result.Hash = ParseHash(std::string_view(stem).substr(0, stem.size() - suffixes[role].size()));
        if (!result.Hash) {
            result.Hash = ParseAzaharTextureFilenameHash(path);
            result.InvalidName = result.Hash.has_value();
        }
        if (result.Hash) result.Role = static_cast<int>(role);
        break;
    }
    return result;
}

std::filesystem::path Sandboxed(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto canonicalRoot = std::filesystem::weakly_canonical(root);
    const auto canonicalPath = std::filesystem::weakly_canonical(path);
    auto parent = canonicalRoot.begin();
    auto child = canonicalPath.begin();
    for (; parent != canonicalRoot.end(); ++parent, ++child) {
        if (child == canonicalPath.end()) throw std::runtime_error("material path escapes the pack directory");
#ifdef _WIN32
        if (Lower(parent->string()) != Lower(child->string()))
#else
        if (*parent != *child)
#endif
            throw std::runtime_error("material path escapes the pack directory");
    }
    return canonicalPath;
}

void Fields(const nlohmann::json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object()) throw std::runtime_error("material section must be an object");
    for (const auto& item : value.items())
        if (std::find(names.begin(), names.end(), item.key()) == names.end())
            throw std::runtime_error("unknown material field: " + item.key());
}

float Number(const nlohmann::json& object, const char* name, float fallback, float maximum = 1.0F) {
    const auto found = object.find(name);
    if (found == object.end()) return fallback;
    if (!found->is_number()) throw std::runtime_error(std::string(name) + " must be numeric");
    const float value = found->get<float>();
    if (!std::isfinite(value) || value < 0.0F || value > maximum)
        throw std::runtime_error(std::string(name) + " is outside its supported range");
    return value;
}

template<size_t N>
void Factor(const nlohmann::json& object, const char* name, std::array<float, N>& output, float maximum = 1.0F) {
    const auto found = object.find(name);
    if (found == object.end()) return;
    if (!found->is_array() || found->size() != N) throw std::runtime_error(std::string(name) + " has the wrong vector size");
    for (size_t component = 0; component < N; ++component) {
        const auto& item = (*found)[component];
        if (!item.is_number()) throw std::runtime_error(std::string(name) + " must contain numbers");
        const float value = item.get<float>();
        if (!std::isfinite(value) || value < 0.0F || value > maximum)
            throw std::runtime_error(std::string(name) + " is outside its supported range");
        output[component] = value;
    }
}

struct Candidate {
    IndexedAzaharPbrMaterial Material;
    std::array<bool, 3> Duplicate{};
    std::array<bool, 3> InvalidName{};
    bool DuplicateJson = false;
};

void Finish(const std::filesystem::path& root, Candidate& candidate) {
    auto& material = candidate.Material;
    if (candidate.DuplicateJson) throw std::runtime_error("multiple material JSON files declare the same hash");
    nlohmann::json document = nlohmann::json::object();
    if (!material.ConfigurationPath.empty()) {
        const auto path = Sandboxed(root, material.ConfigurationPath);
        if (std::filesystem::file_size(path) > 1024U * 1024U) throw std::runtime_error("material JSON exceeds 1 MiB");
        std::ifstream stream(path);
        if (!stream) throw std::runtime_error("unable to open material JSON");
        document = nlohmann::json::parse(stream);
        Fields(document, {"schema", "version", "shading", "baseColor", "normal", "orm", "emissive", "textureSlot"});
        if (document.at("schema") != "triaevum.pbr-material" || !document.at("version").is_number_integer() ||
            document.at("version") != 1 || document.value("shading", "metallic_roughness") != "metallic_roughness")
            throw std::runtime_error("unsupported material schema, version or shading model");
    }
    constexpr std::array<const char*, 3> roles{"normal", "orm", "emissive"};
    for (size_t role = 0; role < roles.size(); ++role) {
        const auto section = document.find(roles[role]);
        if (section != document.end()) {
            if (section->is_null()) {
                material.Textures[role].clear();
                candidate.Duplicate[role] = candidate.InvalidName[role] = false;
            } else {
                if (role == 0) Fields(*section, {"texture", "scale", "convention"});
                if (role == 1) Fields(*section, {"texture", "occlusionStrength", "roughnessFactor", "metallicFactor"});
                if (role == 2) Fields(*section, {"texture", "factor"});
                if (const auto texture = section->find("texture"); texture != section->end()) {
                    candidate.Duplicate[role] = candidate.InvalidName[role] = false;
                    material.Textures[role].clear();
                    if (!texture->is_null()) {
                        if (!texture->is_string()) throw std::runtime_error("texture must be a relative PNG path or null");
                        const auto text = texture->get<std::string>();
                        const std::filesystem::path relative(std::u8string_view(
                            reinterpret_cast<const char8_t*>(text.data()), text.size()));
                        if (text.empty() || text.find('\0') != std::string::npos || relative.is_absolute() || relative.has_root_name())
                            throw std::runtime_error("texture must be a relative PNG path inside the pack");
                        material.Textures[role] = material.ConfigurationPath.parent_path() / relative;
                    }
                }
            }
        }
        if (candidate.Duplicate[role]) throw std::runtime_error(std::string("ambiguous ") + roles[role] + " maps; select one explicitly in JSON");
        if (candidate.InvalidName[role]) throw std::runtime_error("PBR suffixes require HASH_n/orm/e.png, not tex1_..._n/orm/e.png");
        if (!material.Textures[role].empty()) {
            material.Textures[role] = Sandboxed(root, material.Textures[role]);
            if (Lower(material.Textures[role].extension().string()) != ".png")
                throw std::runtime_error("PBR maps currently require PNG files");
            if (!std::filesystem::is_regular_file(material.Textures[role]))
                throw std::runtime_error("material texture does not exist: " + material.Textures[role].string());
        }
    }
    auto& parameters = material.Parameters;
    parameters.MetallicFactor = material.Textures[1].empty() ? 0.0F : 1.0F;
    if (!material.Textures[2].empty()) parameters.EmissiveFactor.fill(1.0F);
    if (const auto base = document.find("baseColor"); base != document.end()) {
        Fields(*base, {"source", "factor"});
        if (base->value("source", "bound_texture") != "bound_texture")
            throw std::runtime_error("baseColor.source must be bound_texture; color replacements use the Azahar catalog");
        Factor(*base, "factor", parameters.BaseColorFactor);
        if (parameters.BaseColorFactor[3] != 1.0F) throw std::runtime_error("PBR factors cannot change native alpha");
    }
    if (const auto normal = document.find("normal"); normal != document.end() && !normal->is_null()) {
        parameters.NormalScale = Number(*normal, "scale", 1.0F, 8.0F);
        const auto convention = normal->value("convention", "+Y");
        if (convention != "+Y" && convention != "-Y") throw std::runtime_error("normal convention must be +Y or -Y");
        parameters.NormalYSign = convention == "+Y" ? 1.0F : -1.0F;
    }
    if (const auto orm = document.find("orm"); orm != document.end() && !orm->is_null()) {
        parameters.OcclusionStrength = Number(*orm, "occlusionStrength", 1.0F);
        parameters.RoughnessFactor = Number(*orm, "roughnessFactor", 1.0F);
        parameters.MetallicFactor = Number(*orm, "metallicFactor", parameters.MetallicFactor);
    }
    if (const auto emission = document.find("emissive"); emission != document.end() && !emission->is_null())
        Factor(*emission, "factor", parameters.EmissiveFactor, 65504.0F);
    if (const auto slot = document.find("textureSlot"); slot != document.end()) {
        if (!slot->is_number_integer() || *slot < 0 || *slot > 2) throw std::runtime_error("textureSlot must be 0, 1 or 2");
        parameters.TextureSlot = slot->get<uint8_t>();
    }
}

} // namespace

std::optional<uint64_t> ParseAzaharTextureFilenameHash(const std::filesystem::path& path) {
    unsigned int width = 0, height = 0, format = 0;
    unsigned long long hash = 0;
    if (std::sscanf(path.stem().string().c_str(), "tex1_%ux%u_%llX_%u", &width, &height, &hash, &format) != 4)
        return std::nullopt;
    return static_cast<uint64_t>(hash);
}

bool IsAzaharPbrSidecar(const std::filesystem::path& path) {
    const auto description = Describe(path);
    return description.Json || description.Role >= 0;
}

std::unordered_map<uint64_t, IndexedAzaharPbrMaterial> IndexAzaharPbrMaterials(
    const std::filesystem::path& root, std::span<const std::filesystem::path> files,
    const std::unordered_map<std::string, std::vector<uint64_t>>& mappedHashes) {
    std::unordered_map<uint64_t, Candidate> candidates;
    for (const auto& path : files) {
        const auto description = Describe(path);
        if (!description.Json && description.Role < 0) continue;
        std::vector<uint64_t> hashes;
        if (description.Hash) hashes.push_back(*description.Hash);
        if (description.LegacyNormal) {
            const auto baseName = Lower(path.stem().stem().string() + path.extension().string());
            if (const auto mapped = mappedHashes.find(baseName); mapped != mappedHashes.end())
                for (const auto hash : mapped->second)
                    if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end()) hashes.push_back(hash);
        }
        for (const auto hash : hashes) {
            auto& candidate = candidates[hash];
            if (description.Json) {
                candidate.DuplicateJson |= !candidate.Material.ConfigurationPath.empty();
                candidate.Material.ConfigurationPath = path;
            } else {
                const auto role = static_cast<size_t>(description.Role);
                candidate.Duplicate[role] |= !candidate.Material.Textures[role].empty();
                candidate.InvalidName[role] |= description.InvalidName;
                candidate.Material.Textures[role] = path;
            }
        }
    }
    std::unordered_map<uint64_t, IndexedAzaharPbrMaterial> result;
    for (auto& [hash, candidate] : candidates) {
        try { Finish(root, candidate); }
        catch (const std::exception& exception) { candidate.Material.Error = exception.what(); }
        result.emplace(hash, std::move(candidate.Material));
    }
    return result;
}

} // namespace Oot3d::Renderer
