// Developer warp runtime against a fake guest: catalog validation, the
// save-context writes of SoH's warps, arrival, void-damage suppression,
// gameplay admission and SoH-format persistence.
#include "oot3d_dev_warp.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using namespace Oot3dNativeGame;

int gChecks = 0;
void Require(bool condition, const std::string &what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

constexpr uint32_t kSave = 0x00587958U;
constexpr uint32_t kPlay = 0x08100000U;
constexpr uint32_t kPlayer = 0x08200000U;
constexpr uint32_t kTable = 0x00543BB8U;
constexpr uint32_t kPlayerHead = kPlay + 0x208CU + 0x0CU + 2U * 8U + 4U;

class FakeGuest final : public DevWarpGuest {
 public:
  bool Read(uint32_t address, std::span<uint8_t> bytes) override {
    for (size_t index = 0; index < bytes.size(); ++index) {
      const auto found = Memory.find(address + static_cast<uint32_t>(index));
      bytes[index] = found == Memory.end() ? 0U : found->second;
    }
    return true;
  }
  bool Write(uint32_t address, std::span<const uint8_t> bytes) override {
    for (size_t index = 0; index < bytes.size(); ++index) Memory[address + static_cast<uint32_t>(index)] = bytes[index];
    return true;
  }
  bool RequestTransition(uint32_t playState, uint16_t entrance, bool *accepted, std::string *) override {
    Requests.push_back({playState, entrance});
    *accepted = Accept;
    if (Accept) Put<int16_t>(playState + 0x5C32U, static_cast<int16_t>(entrance));
    return true;
  }
  template <class T> void Put(uint32_t address, T value) {
    std::array<uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    Write(address, bytes);
  }
  template <class T> T Get(uint32_t address) {
    std::array<uint8_t, sizeof(T)> bytes{};
    Read(address, bytes);
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
  }
  std::unordered_map<uint32_t, uint8_t> Memory;
  std::vector<std::pair<uint32_t, uint16_t>> Requests;
  bool Accept = true;
};

// A game in normal play in `scene` with the player at a known place, and an
// entrance table that agrees with SoH's catalog except where noted.
FakeGuest Game(int16_t scene) {
  FakeGuest guest;
  for (const auto list : {DevWarpVanillaEntrances(), DevWarpBetterEntrances()})
    for (const auto &entrance : list)
      if (entrance.Grotto < 0) {
        guest.Put<uint8_t>(kTable + entrance.Entrance * 4U, entrance.Scene);
        guest.Put<uint8_t>(kTable + entrance.Entrance * 4U + 1U, entrance.Spawn);
      }
  for (const auto &grotto : DevWarpGrottos()) {
    guest.Put<uint8_t>(kTable + grotto.Entrance * 4U, grotto.Scene);
    guest.Put<uint8_t>(kTable + grotto.Entrance * 4U + 1U, grotto.Spawn);
    guest.Put<uint8_t>(kTable + grotto.ReturnEntrance * 4U, grotto.ReturnScene);
    guest.Put<uint8_t>(kTable + grotto.ReturnEntrance * 4U + 1U, grotto.ReturnSpawn);
  }
  guest.Put<uint8_t>(kTable, 0U);
  guest.Put<int32_t>(kSave, 0x00BB);
  guest.Put<int32_t>(kSave + 4U, 1);
  guest.Put<uint16_t>(kSave + 0x0CU, 0x6000U);
  guest.Put<int16_t>(kPlay + 0x104U, scene);
  guest.Put<uint8_t>(kPlay + 0x4C30U, 2U);
  guest.Put<int16_t>(kPlay + 0x5C32U, -1);
  guest.Put<uint32_t>(kPlayerHead, kPlayer);
  guest.Put<float>(kPlayer + 0x28U, 10.0F);
  guest.Put<float>(kPlayer + 0x2CU, 20.0F);
  guest.Put<float>(kPlayer + 0x30U, -30.0F);
  guest.Put<int16_t>(kPlayer + 0xBEU, 0x4000);
  return guest;
}

// The native loader: the requested scene is now the PlayState's.
void Arrive(FakeGuest &guest, DevWarpRuntime &warp, uint16_t entrance, uint8_t scene, uint64_t &frame) {
  warp.Advance(guest, kPlay, frame++);  // transition slot busy
  guest.Put<int16_t>(kPlay + 0x5C32U, -1);
  guest.Put<int32_t>(kSave, entrance);
  guest.Put<int16_t>(kPlay + 0x104U, scene);
  warp.Advance(guest, kPlay, frame++);
}

const DevWarpEntrance &Better(const char *english) {
  for (const auto &entrance : DevWarpBetterEntrances())
    if (std::string(entrance.English) == english) return entrance;
  throw std::runtime_error(std::string("no catalog entrance ") + english);
}

void CatalogAgainstTheTable() {
  auto guest = Game(52);
  DevWarpRuntime warp({});
  const auto &house = Better("Links Bed");
  Require(!warp.Check(house).Known, "availability is known before the table is read");
  uint64_t frame = 1;
  warp.Advance(guest, kPlay, frame++);
  Require(warp.Status().EntranceTableRead, "the entrance table was not read in gameplay");
  Require(warp.Check(house).Available, "a matching SoH entrance is not offered");
  // OOT3D numbers this debug scene differently: shown, not offered.
  guest.Put<uint8_t>(kTable + house.Entrance * 4U, static_cast<uint8_t>(house.Scene + 1U));
  DevWarpRuntime fresh({});
  fresh.Advance(guest, kPlay, frame++);
  const auto mismatch = fresh.Check(house);
  Require(mismatch.Known && !mismatch.Available && mismatch.Reason.find("SoH expects scene") != std::string::npos,
          "a mismatching entrance is offered or unexplained");
}

void SceneEntranceWarp() {
  auto guest = Game(52);
  DevWarpRuntime warp({});
  uint64_t frame = 1;
  warp.Advance(guest, kPlay, frame++);
  const auto &field = Better("Near Drawbridge");
  warp.WarpTo(field, DevWarpAge::Adult, true);
  warp.Advance(guest, kPlay, frame++);
  Require(guest.Requests.size() == 1U && guest.Requests[0].first == kPlay &&
              guest.Requests[0].second == field.Entrance,
          "the native transition was not requested for the entrance");
  Require(guest.Get<uint8_t>(kPlay + 0x5C00U) == 0U, "the age on load was not set to adult");
  Require(guest.Get<uint16_t>(kSave + 0x0CU) == 0x0000U, "night did not set midnight");
  Require(warp.Status().Phase == DevWarpPhase::Requested, "the warp is not in progress");
  Arrive(guest, warp, field.Entrance, field.Scene, frame);
  Require(warp.Status().Phase == DevWarpPhase::Arrived, "the arrival was not observed");
  // The vanilla screen keeps the time of day.
  guest.Put<uint16_t>(kSave + 0x0CU, 0x6000U);
  warp.WarpTo(DevWarpVanillaEntrances()[0], DevWarpAge::Child, std::nullopt);
  for (int step = 0; step < 200 && guest.Requests.size() < 2U; ++step) warp.Advance(guest, kPlay, frame++);
  Require(guest.Requests.size() == 2U && guest.Get<uint16_t>(kSave + 0x0CU) == 0x6000U &&
              guest.Get<uint8_t>(kPlay + 0x5C00U) == 1U,
          "the vanilla warp changed the time or ignored the age");
}

void WarpPointToItsEntrance() {
  auto guest = Game(85);
  DevWarpRuntime warp({});
  uint64_t frame = 1;
  warp.Advance(guest, kPlay, frame++);
  std::string error;
  Require(warp.AddPoint("Bridge", &error), "a warp point was not saved in gameplay: " + error);
  const auto point = warp.Points().at("Bridge");
  Require(point.EntranceId == 0x00BB && point.RoomNum == 2 && point.Position[0] == 10.0F &&
              point.Position[2] == -30.0F && point.RotY == 0x4000,
          "the warp point does not hold the player's place");
  guest.Put<float>(kPlayer + 0x28U, 999.0F);
  warp.WarpToPoint("Bridge");
  warp.Advance(guest, kPlay, frame++);
  Require(guest.Requests.size() == 1U && guest.Requests[0].second == 0x00BB &&
              guest.Get<int32_t>(kSave + 0x4ECU) == 0 && guest.Get<float>(kSave + 0x4F0U) == 0.0F,
          "a warp point did not request its entrance or wrote respawn fields");
  Arrive(guest, warp, 0x00BB, 52, frame);
  Require(warp.Status().Phase == DevWarpPhase::Arrived, "the warp point did not arrive");
}

void GrottoAndAdmission() {
  auto guest = Game(85);
  guest.Put<int32_t>(kSave + 0x14E4U, 1);  // a file menu, not gameplay
  DevWarpRuntime warp({});
  uint64_t frame = 1;
  const auto &storms = Better("Kokiri Forest (Song of Storms)");
  warp.WarpTo(storms, DevWarpAge::Child, false);
  for (int step = 0; step < 5; ++step) warp.Advance(guest, kPlay, frame++);
  Require(guest.Requests.empty() && warp.Status().Phase == DevWarpPhase::Waiting,
          "a warp started outside gameplay");
  guest.Put<int32_t>(kSave + 0x14E4U, 0);
  guest.Put<int16_t>(kPlay + 0x5C32U, 0x0211);  // a transition in progress
  warp.Advance(guest, kPlay, frame++);
  Require(guest.Requests.empty(), "a warp interrupted a native transition");
  guest.Put<int16_t>(kPlay + 0x5C32U, -1);
  warp.Advance(guest, kPlay, frame++);
  Require(guest.Requests.empty() && warp.Status().Phase == DevWarpPhase::Failed &&
              !warp.Check(storms).Available && !warp.Check(storms).Reason.empty(),
          "a grotto warp was offered or requested");
}

void PersistenceAndBoot() {
  const auto path = std::filesystem::temp_directory_path() / "triaevum-dev-warp-test.json";
  std::filesystem::remove(path);
  auto guest = Game(85);
  uint64_t frame = 1;
  {
    DevWarpRuntime warp(path);
    warp.Advance(guest, kPlay, frame++);
    std::string error;
    Require(warp.AddPoint("A", &error) && warp.AddPoint("B", &error), "warp points not saved");
    warp.SetBootPoint("B");
    auto options = warp.Options();
    options.Better = false;
    options.BetterScene = 3;
    warp.SetOptions(options);
  }
  const auto document = nlohmann::json::parse(std::ifstream(path));
  Require(document["WarpPoints"]["B"]["bootToPoint"] == true && document["WarpPoints"]["A"]["bootToPoint"] == false &&
              document["WarpPoints"]["A"].contains("pos") && document["WarpPoints"]["A"]["pos"].contains("x"),
          "warp points are not stored in SoH's format");
  DevWarpRuntime reloaded(path);
  Require(reloaded.Points().size() == 2U && !reloaded.Options().Better && reloaded.Options().BetterScene == 3,
          "the stored points or screen choices were not reloaded");
  // The boot point: warped to when gameplay first starts.
  reloaded.Advance(guest, kPlay, frame++);
  Require(guest.Requests.size() == 1U && guest.Requests[0].second == 0x00BB, "the boot point was not warped to");
  reloaded.RemovePoint("A");
  Require(DevWarpRuntime(path).Points().size() == 1U, "a deleted point came back");
  std::filesystem::remove(path);
}

} // namespace

int main() try {
  CatalogAgainstTheTable();
  SceneEntranceWarp();
  WarpPointToItsEntrance();
  GrottoAndAdmission();
  PersistenceAndBoot();
  std::cout << "dev warp tests passed: " << gChecks << " checks\n";
  return 0;
} catch (const std::exception &error) {
  std::cerr << "dev warp test failed: " << error.what() << '\n';
  return 1;
}
