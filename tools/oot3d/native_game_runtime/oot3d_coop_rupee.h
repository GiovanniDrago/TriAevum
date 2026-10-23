#pragma once

#include <cstdint>
#include <optional>

namespace Oot3dNativeGame {

// En_Item00 stores an Item00Type code in the actor params low byte. These are
// NOT the ITEM_RUPEE_* item ids (0x84..0x88). Values follow the reference
// table: 0x13 (orange) is granted as ITEM_RUPEE_GOLD (200).
inline constexpr uint8_t kCoopItem00RupeeGreen = 0x00U;
inline constexpr uint8_t kCoopItem00RupeeBlue = 0x01U;
inline constexpr uint8_t kCoopItem00RupeeRed = 0x02U;
inline constexpr uint8_t kCoopItem00RupeeOrange = 0x13U;
inline constexpr uint8_t kCoopItem00RupeePurple = 0x14U;

// Link's En_Item00 pickup gate: xz <= 30, y in [-50, +50].
inline constexpr float kCoopRupeePickupRadiusXZ = 30.0F;
inline constexpr float kCoopRupeePickupRadiusY = 50.0F;

// SaveContext wallet upgrade (UPG_WALLET): shift 12, mask 0x3000.
inline constexpr uint32_t kCoopWalletUpgradeShift = 12U;
inline constexpr uint32_t kCoopWalletUpgradeMask = 0x3000U;
inline constexpr uint16_t kCoopWalletCapacities[4] = {99U, 200U, 500U, 500U};

constexpr std::optional<uint16_t> RupeeValueForItemCode(uint8_t itemCode) {
  switch (itemCode) {
    case kCoopItem00RupeeGreen:
      return 1U;
    case kCoopItem00RupeeBlue:
      return 5U;
    case kCoopItem00RupeeRed:
      return 20U;
    case kCoopItem00RupeeOrange:
      return 200U;
    case kCoopItem00RupeePurple:
      return 50U;
    default:
      return std::nullopt;
  }
}

constexpr bool IsRupeeItemCode(uint8_t itemCode) {
  return RupeeValueForItemCode(itemCode).has_value();
}

constexpr uint16_t WalletCapacityForUpgrades(uint32_t upgrades) {
  const uint32_t level =
      (upgrades & kCoopWalletUpgradeMask) >> kCoopWalletUpgradeShift;
  return kCoopWalletCapacities[level < 4U ? level : 3U];
}

// A pickup attempt is accepted only when all gameplay, distance, collectible
// and wallet-capacity conditions hold. The caller must still validate that the
// actor is present in the current lists before invoking guest code.
struct RupeePickupQuery {
  float DistanceXZ = 0.0F;
  float DistanceY = 0.0F;
  int16_t CollectibleDelay = 0;
  bool GameplayActive = false;
  bool CutsceneActive = false;
  bool PlayerAlive = false;
  uint16_t Wallet = 0U;
  uint16_t WalletCapacity = 0U;
  uint16_t PendingCredit = 0U;
  uint16_t RupeeValue = 0U;
};

constexpr bool ShouldCollectRupee(const RupeePickupQuery& query) {
  if (query.RupeeValue == 0U) return false;
  if (!query.GameplayActive || query.CutsceneActive || !query.PlayerAlive) {
    return false;
  }
  if (query.CollectibleDelay > 0) return false;
  if (query.DistanceXZ < 0.0F ||
      query.DistanceXZ > kCoopRupeePickupRadiusXZ) {
    return false;
  }
  if (query.DistanceY < -kCoopRupeePickupRadiusY ||
      query.DistanceY > kCoopRupeePickupRadiusY) {
    return false;
  }
  const uint32_t total = static_cast<uint32_t>(query.Wallet) +
                         static_cast<uint32_t>(query.PendingCredit) +
                         static_cast<uint32_t>(query.RupeeValue);
  return total <= static_cast<uint32_t>(query.WalletCapacity);
}

} // namespace Oot3dNativeGame
