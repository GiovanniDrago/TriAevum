#include "oot3d_interior_sun_prebuild.h"
#include "oot3d_native_room_shadow_shell.h"
#include "recomp/a32_runtime.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>

namespace Oot3dNativeGame {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint64_t kMaxFile = 64ULL * 1024 * 1024;
uint32_t U32(std::span<const uint8_t> b, size_t at) {
  if (at > b.size() || b.size() - at < 4)
    throw std::runtime_error("truncated RomFS/ZSI metadata");
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 |
         uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
uint64_t U64(std::span<const uint8_t> b, size_t at) {
  return U32(b, at) | uint64_t(U32(b, at + 4)) << 32;
}
std::string Name(const Bytes &b, size_t at, size_t n) {
  if (n % 2 || at > b.size() || n > b.size() - at)
    throw std::runtime_error("invalid RomFS name extent");
  std::string s;
  for (size_t i = 0; i < n; i += 2) {
    if (b[at + i + 1] || b[at + i] < 32 || b[at + i] > 126)
      return {};
    s += char(b[at + i]);
  }
  if (s.find_first_of("/\\") != std::string::npos || s == "." || s == "..")
    return {};
  return s;
}
struct Romfs {
  struct File {
    std::filesystem::path Path;
    uint64_t Offset = 0, Size = 0;
  };
  std::map<std::string, File> Files;
  explicit Romfs(const std::filesystem::path &path) {
    if (std::filesystem::is_directory(path)) {
      for (const auto &f : std::filesystem::directory_iterator(path / "scene"))
        if (f.is_regular_file() &&
            f.path().filename().string().ends_with("_info.zsi"))
          Files.emplace(f.path().filename().string(),
                        File{f.path(), 0, f.file_size()});
    } else {
      const auto size = std::filesystem::file_size(path);
      std::ifstream stream(path, std::ios::binary);
      auto read = [&](uint64_t at, uint64_t n) {
        if (at > size || n > size - at || n > 16 * 1024 * 1024)
          throw std::runtime_error("RomFS metadata outside image");
        Bytes b(static_cast<size_t>(n));
        stream.seekg(std::streamoff(at));
        if (!stream.read(reinterpret_cast<char *>(b.data()),
                         std::streamsize(n)))
          throw std::runtime_error("RomFS read failed");
        return b;
      };
      const uint64_t base = U32(read(0, 4), 0) == 0x43465649 ? 0x1000 : 0;
      const auto h = read(base, 40);
      if (U32(h, 0) != 40)
        throw std::runtime_error("invalid RomFS Level-3 header");
      const auto dirs = read(base + U32(h, 12), U32(h, 16)),
                 files = read(base + U32(h, 28), U32(h, 32));
      const uint64_t data = base + U32(h, 36);
      std::vector<uint32_t> sceneDirs;
      for (size_t p = 0; p < dirs.size();) {
        const auto n = U32(dirs, p + 20);
        const auto name = Name(dirs, p + 24, n);
        if (name == "scene" && U32(dirs, p) == 0)
          sceneDirs.push_back(uint32_t(p));
        const uint64_t next = p + ((24ULL + n + 3) & ~3ULL);
        if (next > dirs.size())
          throw std::runtime_error("invalid RomFS directory record");
        p = size_t(next);
      }
      for (size_t p = 0; p < files.size();) {
        const auto n = U32(files, p + 28);
        const auto name = Name(files, p + 32, n);
        const auto offset = U64(files, p + 8), length = U64(files, p + 16);
        if (data > size || offset > size - data ||
            length > size - data - offset)
          throw std::runtime_error("RomFS file outside image");
        if (std::find(sceneDirs.begin(), sceneDirs.end(), U32(files, p)) !=
                sceneDirs.end() &&
            name.ends_with("_info.zsi"))
          if (!Files.emplace(name, File{path, data + offset, length}).second)
            throw std::runtime_error("duplicate RomFS scene file");
        const uint64_t next = p + ((32ULL + n + 3) & ~3ULL);
        if (next > files.size())
          throw std::runtime_error("invalid RomFS file record");
        p = size_t(next);
      }
    }
    if (Files.empty() || Files.size() > 4096)
      throw std::runtime_error("RomFS has no bounded scene inventory");
  }
  Bytes Read(const std::string &name) const {
    const auto found = Files.find(name);
    if (found == Files.end())
      return {};
    const auto &f = found->second;
    if (f.Size < 0x90 || f.Size > kMaxFile)
      throw std::runtime_error("invalid ZSI size: " + name);
    Bytes b(static_cast<size_t>(f.Size));
    std::ifstream in(f.Path, std::ios::binary);
    in.seekg(std::streamoff(f.Offset));
    if (!in.read(reinterpret_cast<char *>(b.data()),
                 std::streamsize(b.size())) ||
        U32(b, 0) != 0x0149535a)
      throw std::runtime_error("invalid ZSI: " + name);
    return b;
  }
};
struct FileMemory : oot3d::recomp::a32::MemoryBus {
  Bytes Data;
  explicit FileMemory(Bytes b) : Data(std::move(b)) {}
  bool Read32(uint32_t at, uint32_t *v) override {
    if (at > Data.size() || Data.size() - at < 4)
      return false;
    *v = U32(Data, at);
    return true;
  }
  bool Read8(uint32_t at, uint8_t *v) override {
    if (at >= Data.size())
      return false;
    *v = Data[at];
    return true;
  }
  bool Write32(uint32_t, uint32_t) override { return false; }
  uint32_t Setups() const {
    uint32_t setups = 0;
    for (size_t at = 0x10; at + 8 <= Data.size() && at < 0x110; at += 8) {
      if (Data[at] == 0x14)
        break;
      if (Data[at] == 0x18)
        setups = std::max(setups, std::min<uint32_t>(Data[at + 1], 16));
    }
    return setups;
  }
};
std::string Hex(uint64_t value) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx",
                static_cast<unsigned long long>(value));
  return b;
}
} // namespace
nlohmann::json
PrebuildInteriorSun(const std::filesystem::path &romfs,
                    const std::filesystem::path &cacheDirectory) {
  using namespace Fast::Renderer3ds;
  if (cacheDirectory.empty())
    throw std::runtime_error("renderer cache directory is required");
  const auto cache =
      std::filesystem::absolute(cacheDirectory).lexically_normal();
  Romfs source(romfs);
  const std::regex roomName(R"(^(.+)_([0-9]+)(_dd)?_info\.zsi$)");
  auto entries = nlohmann::json::array();
  size_t roomCount = 0, built = 0, hits = 0, fallback = 0;
  std::map<std::pair<uint64_t, bool>, nlohmann::json> completed;
  for (const auto &[name, file] : source.Files) {
    std::smatch match;
    if (!std::regex_match(name, match, roomName))
      continue;
    ++roomCount;
    FileMemory room(source.Read(name));
    FileMemory scene(
        source.Read(match[1].str() + match[3].str() + "_info.zsi"));
    const uint32_t setups = std::max(room.Setups(), scene.Setups());
    NativeLoadedRoom loaded{
        0, NativeRoomFingerprint(std::span(room.Data).subspan(0x10, 128)),
        0x10};
    for (uint32_t setup = 0; setup <= setups; ++setup) {
      const auto started = std::chrono::steady_clock::now();
      const auto header = ReadNativeSunHeader(0x10, setup, room);
      const bool interior =
          NativeRoomIsInterior(header, ReadNativeSunHeader(0x10, setup, scene));
      uint64_t fingerprint = NativeRoomSunFingerprint(
          loaded.Fingerprint, header ? header->CommandOffset : 0);
      auto entry = nlohmann::json{{"room", name},
                                  {"setup", setup},
                                  {"interior", interior},
                                  {"status", "fallback"}};
      std::string error;
      auto cmbs = CaptureNativeRoomShadowCmbs(
          loaded, room, NativeRoomShadowSource::File, &error, setup);
      std::shared_ptr<const RoomShadowShell> mesh;
      RoomInteriorSunStats stats;
      if (cmbs) {
        fingerprint = NativeRoomSunContentFingerprint(fingerprint, *cmbs);
        const auto key = std::pair{fingerprint, interior};
        if (const auto found = completed.find(key); found != completed.end()) {
          auto reused = found->second;
          reused["room"] = name;
          reused["setup"] = setup;
          reused["reused_in_run"] = true;
          entries.push_back(std::move(reused));
          continue;
        }
        mesh = LoadRoomShadowShell(cache, fingerprint, interior);
        if (mesh) {
          entry["status"] = "hit";
          ++hits;
        } else {
          std::vector<RoomShadowTriangle> triangles;
          bool complete = true;
          for (const auto &bytes : *cmbs) {
            const auto geometry = ParseNativeRoomShadowCmb(bytes);
            if (!geometry || !geometry->Complete ||
                triangles.size() + geometry->Triangles.size() >
                    kMaximumRoomShadowTriangles) {
              complete = false;
              error = "unsupported or incomplete CMB geometry";
              break;
            }
            triangles.insert(triangles.end(), geometry->Triangles.begin(),
                             geometry->Triangles.end());
          }
          if (complete) {
            mesh = BuildRoomShadowShell(fingerprint, triangles,
                                        kRoomShadowRasterSize, {}, interior,
                                        &stats);
            if (!mesh)
              error = "interior-sun work or memory bound exceeded";
          }
          if (mesh) {
            if (!SaveRoomShadowShell(cache, *mesh, &error))
              throw std::runtime_error("interior-sun cache write failed: " +
                                       error);
            entry["status"] = "built";
            ++built;
          }
        }
      }
      entry["fingerprint"] = Hex(fingerprint);
      entry["voxels"] = stats.Voxels;
      entry["peak_grid_bytes"] = stats.PeakWorkingBytes;
      entry["interior_voxels"] = stats.InteriorVoxels;
      entry["closing_steps"] = stats.ClosingSteps;
      entry["ms"] = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count();
      if (mesh) {
        entry["path"] =
            RoomShadowShellPath({}, fingerprint, interior).generic_string();
        entry["triangles"] = mesh->ShellMasks.size();
      } else {
        ++fallback;
        entry["reason"] = error;
      }
      completed.emplace(std::pair{fingerprint, interior}, entry);
      std::fprintf(stderr,
                   "TRIAEVUM_INTERIOR_SUN room=%s setup=%u interior=%d "
                   "status=%s ms=%.3f voxels=%llu\n",
                   name.c_str(), setup, interior,
                   entry["status"].get<std::string>().c_str(),
                   entry["ms"].get<double>(),
                   static_cast<unsigned long long>(stats.Voxels));
      entries.push_back(std::move(entry));
    }
  }
  if (!roomCount)
    throw std::runtime_error("RomFS contains no room files");
  return {{"format", "triaevum_interior_sun_v1"},
          {"criterion_version", kRoomShadowShellVersion},
          {"game_booted", false},
          {"cache_directory", cache.generic_string()},
          {"rooms", roomCount},
          {"built", built},
          {"hits", hits},
          {"fallback", fallback},
          {"entries", std::move(entries)}};
}
nlohmann::json ListNativeEnvironmentGroups(const std::filesystem::path& romfs) {
  // Names/IDs only, from scene_lights.py's title table. Groups are exclusively
  // decoded from each resource's selected authored command lists below.
  static const std::map<std::string, uint16_t> sceneIds{
      {"ydan", 0},
      {"ddan", 1},
      {"bdan", 2},
      {"bmori1", 3},
      {"hidan", 4},
      {"mizusin", 5},
      {"jyasinzou", 6},
      {"hakadan", 7},
      {"hakadan_ch", 8},
      {"ice_doukutu", 9},
      {"ganon", 10},
      {"men", 11},
      {"gerudoway", 12},
      {"ganontika", 13},
      {"ganon_sonogo", 14},
      {"ganontikasonogo", 15},
      {"takaraya", 16},
      {"ydan_boss", 17},
      {"ddan_boss", 18},
      {"bdan_boss", 19},
      {"moriboss", 20},
      {"fire_bs", 21},
      {"mizusin_boss", 22},
      {"jyasinzou_boss", 23},
      {"hakadan_boss", 24},
      {"ganon_boss", 25},
      {"ganon_final", 26},
      {"entra_day", 27},
      {"entra_night", 28},
      {"entra_ruins", 29},
      {"market_alley", 30},
      {"market_alley_n", 31},
      {"market_day", 32},
      {"market_night", 33},
      {"market_ruins", 34},
      {"shrine", 35},
      {"shrine_n", 36},
      {"shrine_r", 37},
      {"k_home", 38},
      {"k_home3", 39},
      {"k_home4", 40},
      {"k_home5", 41},
      {"kakariko", 42},
      {"kakariko_home3", 43},
      {"shop", 44},
      {"kokiri", 45},
      {"shop_golon", 46},
      {"zoora", 47},
      {"shop_drag", 48},
      {"shop_alley", 49},
      {"shop_night", 50},
      {"shop_face", 51},
      {"link", 52},
      {"kakariko_impa", 53},
      {"stable", 54},
      {"labo", 55},
      {"hylia_labo", 56},
      {"tent", 57},
      {"hut", 58},
      {"daiyousei_izumi", 59},
      {"yousei_izumi_tate", 60},
      {"yousei_izumi_yoko", 61},
      {"kakusiana", 62},
      {"hakaana", 63},
      {"hakaana2", 64},
      {"hakaana_ouke", 65},
      {"syatekijyou", 66},
      {"tokinoma", 67},
      {"kenjyanoma", 68},
      {"hairal_niwa", 69},
      {"hairal_niwa_n", 70},
      {"hiral_demo", 71},
      {"hakasitarelay", 72},
      {"turibori", 73},
      {"nakaniwa", 74},
      {"bowling", 75},
      {"souko", 76},
      {"miharigoya", 77},
      {"mahouya", 78},
      {"ganon_demo", 79},
      {"kinsuta", 80},
      {"spot00", 81},
      {"spot01", 82},
      {"spot02", 83},
      {"spot03", 84},
      {"spot04", 85},
      {"spot05", 86},
      {"spot06", 87},
      {"spot07", 88},
      {"spot08", 89},
      {"spot09", 90},
      {"spot10", 91},
      {"spot11", 92},
      {"spot12", 93},
      {"spot13", 94},
      {"spot15", 95},
      {"spot16", 96},
      {"spot17", 97},
      {"spot18", 98},
      {"spot20", 99},
      {"ganon_tou", 100},
      {"spot99", 107}};
  Romfs source(romfs);
  const std::regex roomName(R"(^(.+)_([0-9]+)(_dd)?_info\.zsi$)");
  std::map<std::pair<std::string,uint32_t>, nlohmann::json> rows;
  for (const auto& [name, unused] : source.Files) {
    std::smatch match;
    if (!std::regex_match(name, match, roomName)) continue;
    const auto stem = match[1].str(), variant = match[3].str();
    FileMemory room(source.Read(name)), scene(source.Read(stem + variant + "_info.zsi"));
    for (uint32_t setup=0; setup<=std::max(room.Setups(),scene.Setups()); ++setup) {
      auto& row = rows[{stem+variant,setup}];
      if (row.is_null()) {
        row = {{"scene",stem},{"variant",variant.empty()?"standard":"master-quest"},{"setup",setup},
               {"policy","first-installed-room-per-visit"},{"rooms",nlohmann::json::array()}};
        if (const auto id=sceneIds.find(stem); id!=sceneIds.end()) row["sceneId"]=id->second;
      }
      const auto header=ReadNativeSunHeader(0x10,setup,room);
      const auto group=ClassifyNativeEnvironmentGroup(header,ReadNativeSunHeader(0x10,setup,scene));
      nlohmann::json item={{"room",std::stoul(match[2].str())},{"file",name},
          {"group",group?nlohmann::json(EnvironmentGroupId(*group)):nlohmann::json(nullptr)}};
      if (header) {
        if (header->TimeHour) item["hour"]=*header->TimeHour;
        if (header->TimeMinute) item["minute"]=*header->TimeMinute;
        if (header->TimeSpeed) item["speed"]=*header->TimeSpeed;
      }
      row["rooms"].push_back(std::move(item));
    }
  }
  auto entries=nlohmann::json::array();
  for (auto& [key,row] : rows) {
    auto& rooms=row["rooms"];
    std::sort(rooms.begin(),rooms.end(),[](const auto& a,const auto& b){ return a.at("room")<b.at("room"); });
    row["firstRoomGroup"]=rooms.front()["group"];
    row["mixed"]=std::any_of(rooms.begin(),rooms.end(),[&](const auto& r){return r.at("group")!=rooms.front()["group"];});
    entries.push_back(std::move(row));
  }
  return {{"schema","triaevum.environment-groups"},{"version",1},{"gameBooted",false},{"entries",entries}};
}
int InteriorSunToolMain(int argc, char **argv) {
  std::filesystem::path romfs, cache, manifest;
  for (int i = 2; i < argc; i += 2) {
    if (i + 1 == argc)
      throw std::runtime_error("missing interior-sun tool argument");
    const std::string_view option = argv[i];
    auto *target = option == "--romfs"                      ? &romfs
                   : option == "--renderer-cache-directory" ? &cache
                   : option == "--manifest"                 ? &manifest
                                                            : nullptr;
    if (!target || !target->empty())
      throw std::runtime_error(
          "unknown or repeated interior-sun tool argument");
    *target = argv[i + 1];
  }
  if (romfs.empty() || cache.empty() || manifest.empty())
    throw std::runtime_error("usage: --prebuild-interior-sun --romfs PATH "
                             "--renderer-cache-directory PATH --manifest PATH");
  const auto result = PrebuildInteriorSun(romfs, cache);
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  out << result.dump(2) << '\n';
  out.close();
  if (!out)
    throw std::runtime_error("cannot write interior-sun manifest");
  return 0;
}
} // namespace Oot3dNativeGame
