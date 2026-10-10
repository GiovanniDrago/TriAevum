#include "oot3d_native_a32_lighting.h"
#include "oot3d_native_pica_composition.h"
#include "oot3d_native_room_shadow_shell.h"
#include "oot3d_typed_gameplay_bridge.h"
#include "recomp/a32_runtime.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace Oot3dNativeGame;
using namespace Fast::Renderer3ds;
void Check(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(what);
}
void Put(std::vector<uint8_t> &b, size_t o, uint32_t value, size_t n = 4) {
  for (size_t i = 0; i < n; ++i)
    b.at(o + i) = uint8_t(value >> (i * 8));
}
void Float(std::vector<uint8_t> &b, size_t o, float value) {
  Put(b, o, std::bit_cast<uint32_t>(value));
}
void Magic(std::vector<uint8_t> &b, size_t o, std::string_view value) {
  std::copy(value.begin(), value.end(), b.begin() + o);
}
std::vector<uint8_t> Cmb(uint16_t indexType = 0x1403) {
  std::vector<uint8_t> b(0x800);
  Magic(b, 0, "cmb ");
  Put(b, 4, uint32_t(b.size()));
  Put(b, 8, 6);
  Put(b, 0x20, 8);
  Put(b, 0x24, 0x50);
  Put(b, 0x28, 0x100);
  Put(b, 0x30, 0x280);
  Put(b, 0x38, 0x500);
  Put(b, 0x3c, 0x600);
  Magic(b, 0x50, "skl ");
  Put(b, 0x58, 2);
  for (size_t i = 0; i < 2; ++i) {
    size_t o = 0x60 + i * 0x28;
    Put(b, o, uint32_t(i), 2);
    Put(b, o + 2, i ? 0 : 0xffff, 2);
    Float(b, o + 4, 1);
    Float(b, o + 8, 1);
    Float(b, o + 12, 1);
  }
  Float(b, 0x60 + 28, 10);
  Float(b, 0x88 + 32, 20);
  Magic(b, 0x100, "mats");
  Put(b, 0x108, 1);
  b[0x10c + 4] = 1;
  Magic(b, 0x280, "sklm");
  Put(b, 0x288, 0x20);
  Put(b, 0x28c, 0x40);
  Magic(b, 0x2a0, "mshs");
  Put(b, 0x2a8, 1);
  Magic(b, 0x2c0, "shp ");
  Put(b, 0x2c8, 1);
  Put(b, 0x2d0, 0x20, 2);
  Magic(b, 0x2e0, "sepd");
  Put(b, 0x2e8, 1, 2);
  Put(b, 0x2ea, 1, 2);
  Float(b, 0x2e0 + 0x28, 1);
  Put(b, 0x2e0 + 0x2c, 0x1406, 2);
  Put(b, 0x2e0 + 0x108, 0x120, 2);
  Magic(b, 0x400, "prms");
  Put(b, 0x40e, 1, 2);
  Put(b, 0x410, 0x20);
  Put(b, 0x414, 0x28);
  Put(b, 0x420, 1, 2);
  Magic(b, 0x428, "prm ");
  Put(b, 0x430, 1);
  Put(b, 0x438, indexType, 2);
  Put(b, 0x43c, 3, 2);
  Put(b, 0x43e, 1, 2);
  Magic(b, 0x500, "vatr");
  Put(b, 0x50c, 36);
  Put(b, 0x510, 0x50);
  Float(b, 0x55c, 1);
  Float(b, 0x570, 1);
  b[0x600] = 255; // first index is in 16-bit units even for U8/U32
  const size_t n = indexType == 0x1401 ? 1 : indexType == 0x1403 ? 2 : 4;
  for (size_t i = 0; i < 3; ++i)
    Put(b, 0x602 + i * n, uint32_t(i), n);
  return b;
}
struct Memory : oot3d::recomp::a32::MemoryBus {
  std::vector<uint8_t> Bytes;
  uint32_t Reads = 0;
  uint32_t Base = 0;
  explicit Memory(size_t size) : Bytes(size) {}
  bool Read32(uint32_t at, uint32_t *value) override {
    ++Reads;
    if (at < Base || uint64_t(at - Base) + 4 > Bytes.size())
      return false;
    at -= Base;
    *value = 0;
    for (size_t i = 0; i < 4; ++i)
      *value |= uint32_t(Bytes[at + i]) << (i * 8);
    return true;
  }
  bool Write32(uint32_t, uint32_t) override { return false; }
};
void UnitTests() {
  for (auto kind : {0x1401, 0x1403, 0x1405}) {
    auto b = Cmb(uint16_t(kind));
    auto mesh = ParseNativeRoomShadowCmb(b);
    Check(mesh && mesh->Complete && mesh->Triangles.size() == 1,
          "rigid indexed triangle missing");
    Check(mesh->Triangles[0].Positions[0] == ShadowVector{10, 20, 0},
          "parent bind chain not applied");
    Check(mesh->Triangles[0].Positions[1] == ShadowVector{11, 20, 0},
          "index start units wrong");
    Float(b, 0x88 + 4, 2);
    Float(b, 0x88 + 12, 4);
    Float(b, 0x88 + 24, 1.57079632679F);
    mesh = ParseNativeRoomShadowCmb(b);
    Check(bool(mesh), "TRS model rejected");
    Check(std::abs(mesh->Triangles[0].Positions[1][1] - 22) < .001F &&
              mesh->Triangles[0].Positions[2][2] == 4,
          "Rz * Ry * Rx * S transform wrong");
  }
  auto b = Cmb();
  Put(b, 0x60 + 2, 1, 2);
  Check(!ParseNativeRoomShadowCmb(b), "bone cycle accepted");
  b = Cmb();
  Put(b, 0x88 + 2, 3, 2);
  Check(!ParseNativeRoomShadowCmb(b), "invalid parent accepted");
  b = Cmb();
  Float(b, 0x550, std::numeric_limits<float>::quiet_NaN());
  Check(!ParseNativeRoomShadowCmb(b), "NaN accepted");
  b = Cmb();
  Put(b, 0x2e0 + 0x24, 4096);
  Check(!ParseNativeRoomShadowCmb(b), "invalid position stream accepted");
  b = Cmb();
  Put(b, 0x43e, 0xffff, 2);
  Check(!ParseNativeRoomShadowCmb(b), "invalid index range accepted");
  b = Cmb();
  Put(b, 0x40c, 1, 2);
  auto mesh = ParseNativeRoomShadowCmb(b);
  Check(mesh && !mesh->Complete && mesh->Triangles.empty(),
        "skinned geometry replaced");
  b = Cmb();
  b[0x10c + 0x130] = 1;
  mesh = ParseNativeRoomShadowCmb(b);
  Check(mesh && mesh->Triangles.empty(), "alpha geometry cached");
  b = Cmb();
  Put(b, 0x10c + 0x138, 1);
  mesh = ParseNativeRoomShadowCmb(b);
  Check(mesh && mesh->Triangles.empty(), "blended geometry cached");
  b = Cmb();
  b[0x110] = 0;
  mesh = ParseNativeRoomShadowCmb(b);
  Check(mesh && !mesh->Triangles[0].CullBack, "two-sided flag lost");
  b = Cmb();
  b.resize(0x700);
  Check(!ParseNativeRoomShadowCmb(b), "truncation accepted");
  // Real room command/table layout, with no guest file length available.
  Memory memory(0x2000);
  const uint32_t base = 0x100;
  Put(memory.Bytes, base, 0x0a);
  Put(memory.Bytes, base + 4, 0x80);
  Put(memory.Bytes, base + 8, 0x14);
  memory.Bytes[base + 0x80] = 2;
  memory.Bytes[base + 0x81] = 1;
  Put(memory.Bytes, base + 0x84, 0x90);
  Put(memory.Bytes, base + 0x98, 0x100);
  b = Cmb();
  std::copy(b.begin(), b.end(), memory.Bytes.begin() + base + 0x100);
  NativeLoadedRoom room{
      0, NativeRoomFingerprint(std::span(memory.Bytes).subspan(base, 128)),
      base};
  auto captured =
      CaptureNativeRoomShadowCmbs(room, memory, NativeRoomShadowSource::File);
  Check(captured && captured->size() == 1 && captured->front() == b,
        "bounded room capture failed");
  // The installed ZSI mesh header has an absolute entries pointer, even
  // though its command argument and model-entry offsets stay relative.
  Put(memory.Bytes, base + 0x84, base + 0x90);
  captured = CaptureNativeRoomShadowCmbs(room, memory);
  Check(captured && captured->size() == 1 && captured->front() == b,
        "installed room's relocated entry table was rebased twice");
  Put(memory.Bytes, base + 0x84, base - 4);
  std::string error;
  Check(!CaptureNativeRoomShadowCmbs(
            room, memory, NativeRoomShadowSource::Installed, &error) &&
            !error.empty(),
        "out-of-room relocated pointer accepted or unreported");
  Put(memory.Bytes, base + 0x84, base + 0x90);
  NativeRoomShadowShells cache;
  auto reads = memory.Reads;
  Check(!cache.Capture(false, 123, std::span(&room, 1), memory) &&
            memory.Reads == reads,
        "disabled shell cache accessed guest");
  auto scene = cache.Capture(true, 123, std::span(&room, 1), memory);
  Check(scene && scene->Rooms.size() == 1, "room snapshot not published");
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!scene->Ready() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    scene = cache.Capture(true, 123, std::span(&room, 1), memory);
  }
  Check(scene->Ready() && scene->Rooms[0].Fingerprint == room.Fingerprint,
        "worker did not publish mesh");
  reads = memory.Reads;
  Check(cache.Capture(true, 123, std::span(&room, 1), memory) == scene &&
            memory.Reads == reads,
        "unchanged room recopied or republished");
  Check(!cache.Capture(true, 123, {}, memory),
        "unloaded room snapshot retained");
  // Exercise the real lighting-probe publication boundary as well as the
  // cache: toggling the flag after a room is already loaded must publish it.
  memory.Bytes.resize(0x590000);
  constexpr uint32_t play = 0x1000;
  Put(memory.Bytes, play + 0x4ffc, base);
  memory.Bytes[play + 0x500c] = 0xff;
  NativeA32LightingProbe lighting;
  lighting.SetStaticEmitters({});
  lighting.CaptureSceneLights(play, memory);
  Check(!lighting.CaptureRenderer(0, memory).RoomShadows,
        "disabled probe published shells");
  lighting.SetRoomShadowShellsEnabled(true);
  auto publication = lighting.CaptureRenderer(0, memory);
  const auto readyUntil =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    lighting.CaptureSceneLights(play, memory);
    publication = lighting.CaptureRenderer(0, memory);
    if (publication.RoomShadows && publication.RoomShadows->Ready())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < readyUntil);
  Check(publication.RoomShadows && publication.RoomShadows->Ready() &&
            publication.RoomShadows->Rooms.front().Fingerprint ==
                room.Fingerprint,
        "enabled lighting probe did not publish the installed room");
  lighting.SetRoomShadowShellsEnabled(false);
  lighting.CaptureSceneLights(play, memory);
  Check(!lighting.CaptureRenderer(0, memory).RoomShadows,
        "disabled probe retained shell publication");
  // Exercise the gameplay hook -> CMB packet span -> frontend draw boundary.
  // In particular the ready worker snapshot must replace an earlier pending
  // snapshot even when the geometry and command-list addresses stay fixed.
  Oot3dNativePicaCompositionTracker tracker;
  tracker.SetRoomShadowShellsEnabled(true);
  constexpr uint32_t packet = 0x800, commands = 0x2000;
  Put(memory.Bytes, packet + 0x10, 8);
  Put(memory.Bytes, 0x54cc4c, commands);
  const auto captureDraw = [&]() {
    oot3d::recomp::a32::GuestState state{};
    state.r[0] = play;
    tracker.ObserveBlockEntry(kOot3dGameplayDrawEntry, state, memory);
    state.r[0] = 0;
    state.r[1] = 0;
    state.r[14] = 0x002FADDC;
    tracker.ObserveBlockEntry(kOot3dCmbRendererSubmitDrawHandleEntry, state,
                              memory);
    state.r[0] = packet;
    tracker.ObserveBlockEntry(kOot3dMeshCommandPacketSubmitEntry, state,
                              memory);
    tracker.ObserveBlockEntry(0x002FADDC, state, memory);
    std::vector<Oot3dPicaCommandListCompositionSpan> spans;
    Check(tracker.TakeCommandListCompositionSpans(commands, 8, spans) &&
              spans.size() == 1,
          "CMB packet lost its command span");
    auto frontend = std::make_unique<Oot3dNativePicaFrontend>();
    Check(frontend->SetNextCommandListCompositionSpans(commands, 8, spans),
          "frontend rejected room span");
    Oot3dGspCommandPacket submit{};
    submit.Control = 1;
    submit.Parameters[0] = commands;
    submit.Parameters[1] = 8;
    const std::array<uint32_t, 2> words{1, 0x000f022e};
    Check(frontend->SubmitGspCommand(submit, words) &&
              frontend->PendingDrawPackets().size() == 1,
          "frontend lost room draw");
    const auto &draw = frontend->PendingDrawPackets().front();
    Check(draw.CompositionDomain == Oot3dPicaCompositionDomain::Scene &&
              draw.Composition.Layer ==
                  Oot3dPicaCompositionLayer::OpaqueWorld &&
              draw.Composition.Provenance ==
                  Oot3dPicaCompositionProvenance::NativeCmbDrawPass &&
              !draw.Composition.NativeActorGeometry &&
              draw.LightBinding.RoomShadows ==
                  spans.front().LightBinding.RoomShadows,
          "room ownership or shell snapshot lost between native hook and "
          "frontend");
    return draw.LightBinding.RoomShadows;
  };
  std::shared_ptr<const RoomShadowShellScene> frontendScene;
  const auto frontendReadyUntil =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    frontendScene = captureDraw();
    if (frontendScene && frontendScene->Ready())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < frontendReadyUntil);
  Check(frontendScene && frontendScene->Ready() &&
            frontendScene->Rooms.front().Fingerprint == room.Fingerprint,
        "worker readiness never reached frontend draw");
  Check(captureDraw() == frontendScene,
        "unchanged frontend room snapshot was not shared");
  tracker.SetRoomShadowShellsEnabled(false);
  Check(!captureDraw(), "frontend retained a disabled room snapshot");
  Put(memory.Bytes, base + 0x98, 0xffffff00);
  Check(!CaptureNativeRoomShadowCmbs(room, memory),
        "unbounded room pointer accepted");
}
// Optional read-only ROM validation; never starts the title or writes assets.
void RomCheck(const std::filesystem::path &sceneDirectory) {
  size_t rooms = 0, models = 0, triangles = 0, incomplete = 0;
  for (const auto &file : std::filesystem::directory_iterator(sceneDirectory)) {
    const auto name = file.path().filename().string();
    if (!name.ends_with("_info.zsi"))
      continue;
    std::ifstream in(file.path(), std::ios::binary | std::ios::ate);
    const auto length = in.tellg();
    if (length < 0)
      continue;
    Memory memory(size_t(length) + 0x1000);
    in.seekg(0);
    in.read(reinterpret_cast<char *>(memory.Bytes.data() + 0x100), length);
    if (!in)
      throw std::runtime_error("ROM read failed");
    const uint32_t base = 0x110;
    NativeLoadedRoom room{
        0, NativeRoomFingerprint(std::span(memory.Bytes).subspan(base, 128)),
        base};
    // Scene (as opposed to room) files have no mesh command.
    auto captured =
        CaptureNativeRoomShadowCmbs(room, memory, NativeRoomShadowSource::File);
    if (!captured)
      continue;
    ++rooms;
    for (const auto &bytes : *captured) {
      auto geometry = ParseNativeRoomShadowCmb(bytes);
      if (!geometry)
        throw std::runtime_error("ROM CMB rejected: " + name);
      ++models;
      triangles += geometry->Triangles.size();
      incomplete += !geometry->Complete;
    }
  }
  Check(rooms > 0, "ROM room validation found no models");
  std::printf(
      "ROM parser: %zu rooms, %zu models, %zu triangles, %zu incomplete\n",
      rooms, models, triangles, incomplete);
}
// Optional offline capture: little-endian guest base followed by installed
// room bytes. No ROM data is committed; this exercises a real saved room.
void GuestRoomCheck(const std::filesystem::path &file) {
  std::ifstream in(file, std::ios::binary | std::ios::ate);
  const auto size = in.tellg();
  Check(size > 132 && size <= 64 * 1024 * 1024, "invalid guest room fixture");
  Memory memory(size_t(size) - 4);
  in.seekg(0);
  std::array<uint8_t, 4> base{};
  in.read(reinterpret_cast<char *>(base.data()), 4);
  for (size_t i = 0; i < 4; ++i)
    memory.Base |= uint32_t(base[i]) << (8 * i);
  in.read(reinterpret_cast<char *>(memory.Bytes.data()), memory.Bytes.size());
  Check(bool(in), "guest room read failed");
  NativeLoadedRoom room{
      0, NativeRoomFingerprint(std::span(memory.Bytes).first(128)),
      memory.Base};
  auto captured = CaptureNativeRoomShadowCmbs(room, memory);
  Check(bool(captured), "installed savestate room rejected");
  NativeRoomShadowShells cache;
  auto scene = cache.Capture(true, 0, std::span(&room, 1), memory);
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!scene->Ready() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    scene = cache.Capture(true, 0, std::span(&room, 1), memory);
  }
  Check(scene->Ready() && !scene->Rooms.front().Mesh->Indices.empty(),
        "saved room worker did not produce a mesh");
  std::printf("installed room: fingerprint=%016llx triangles=%zu ready=1\n",
              static_cast<unsigned long long>(room.Fingerprint),
              scene->Rooms.front().Mesh->ShellMasks.size());
}
} // namespace
int main(int argc, char **argv) try {
  UnitTests();
  if (argc == 2)
    RomCheck(argv[1]);
  if (argc == 3 && std::string_view(argv[1]) == "--guest-room")
    GuestRoomCheck(argv[2]);
  std::puts("native room shadow shell tests passed");
  return 0;
} catch (const std::exception &e) {
  std::fprintf(stderr, "FAILED: %s\n", e.what());
  return 1;
}
