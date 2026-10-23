// Pure unit tests for the co-op rupee mapping and pickup gate. These tests do
// not boot the guest runtime: they pin the item-code table, the Link-parity
// distance gate, the collectible-delay gate, and the wallet capacity table.
#include "oot3d_coop_rupee.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace Oot3dNativeGame;

int gChecks = 0;

void Require(bool condition, const std::string& what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

void TestItemCodeMap() {
  Require(RupeeValueForItemCode(0x00U) == std::optional<uint16_t>(1U),
          "green rupee must be worth 1");
  Require(RupeeValueForItemCode(0x01U) == std::optional<uint16_t>(5U),
          "blue rupee must be worth 5");
  Require(RupeeValueForItemCode(0x02U) == std::optional<uint16_t>(20U),
          "red rupee must be worth 20");
  Require(RupeeValueForItemCode(0x13U) == std::optional<uint16_t>(200U),
          "orange rupee must map to the 200-value gold rupee");
  Require(RupeeValueForItemCode(0x14U) == std::optional<uint16_t>(50U),
          "purple rupee must be worth 50");
  Require(!RupeeValueForItemCode(0x11U).has_value(),
          "0x11 is a small key, not a rupee");
  Require(!RupeeValueForItemCode(0x03U).has_value(),
          "0x03 is a recovery heart, not a rupee");
  Require(!RupeeValueForItemCode(0x84U).has_value(),
          "0x84 is an item id, not an En_Item00 code");
}

void TestDistanceGate() {
  RupeePickupQuery query{};
  query.GameplayActive = true;
  query.PlayerAlive = true;
  query.Wallet = 10U;
  query.WalletCapacity = 99U;
  query.RupeeValue = 1U;
  query.DistanceXZ = 30.0F;
  query.DistanceY = 50.0F;
  query.CollectibleDelay = 0;
  Require(ShouldCollectRupee(query), "exact Link-parity boundary must collect");

  query.DistanceXZ = 30.01F;
  Require(!ShouldCollectRupee(query), "beyond 30 units must not collect");

  query.DistanceXZ = 30.0F;
  query.DistanceY = -50.01F;
  Require(!ShouldCollectRupee(query), "below -50 vertical must not collect");

  query.DistanceY = 50.01F;
  Require(!ShouldCollectRupee(query), "above +50 vertical must not collect");
}

void TestStateGate() {
  RupeePickupQuery query{};
  query.GameplayActive = true;
  query.PlayerAlive = true;
  query.Wallet = 10U;
  query.WalletCapacity = 99U;
  query.RupeeValue = 5U;
  query.DistanceXZ = 10.0F;
  query.DistanceY = 0.0F;
  query.CollectibleDelay = 1;
  Require(!ShouldCollectRupee(query), "collectible delay must block pickup");

  query.CollectibleDelay = 0;
  query.CutsceneActive = true;
  Require(!ShouldCollectRupee(query), "cutscenes must block pickup");

  query.CutsceneActive = false;
  query.GameplayActive = false;
  Require(!ShouldCollectRupee(query), "non-gameplay modes must block pickup");

  query.GameplayActive = true;
  query.PlayerAlive = false;
  Require(!ShouldCollectRupee(query), "game-over must block pickup");
}

void TestWalletGate() {
  RupeePickupQuery query{};
  query.GameplayActive = true;
  query.PlayerAlive = true;
  query.WalletCapacity = 99U;
  query.DistanceXZ = 10.0F;
  query.DistanceY = 0.0F;
  query.CollectibleDelay = 0;

  query.Wallet = 98U;
  query.PendingCredit = 0U;
  query.RupeeValue = 1U;
  Require(ShouldCollectRupee(query), "exact fit must collect");

  query.Wallet = 98U;
  query.PendingCredit = 1U;
  query.RupeeValue = 1U;
  Require(!ShouldCollectRupee(query), "pending credit must count against capacity");

  query.Wallet = 90U;
  query.PendingCredit = 0U;
  query.RupeeValue = 20U;
  Require(!ShouldCollectRupee(query), "value larger than remaining room must be rejected");

  query.Wallet = 79U;
  query.PendingCredit = 0U;
  query.RupeeValue = 20U;
  Require(ShouldCollectRupee(query), "value equal to remaining room must collect");
}

void TestWalletCapacityTable() {
  Require(WalletCapacityForUpgrades(0x0U) == 99U, "level 0 wallet is 99");
  Require(WalletCapacityForUpgrades(0x1000U) == 200U, "level 1 wallet is 200");
  Require(WalletCapacityForUpgrades(0x2000U) == 500U, "level 2 wallet is 500");
  Require(WalletCapacityForUpgrades(0x3000U) == 500U, "level 3 wallet is 500");
  Require(WalletCapacityForUpgrades(0xFFFFU) == 500U,
          "out-of-range upgrade bits must clamp to the largest wallet");
}

} // namespace

int main() {
  try {
    TestItemCodeMap();
    TestDistanceGate();
    TestStateGate();
    TestWalletGate();
    TestWalletCapacityTable();
  } catch (const std::exception& error) {
    std::cerr << "coop rupee tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "coop rupee tests passed (" << gChecks << " checks)\n";
  return 0;
}
