#include "oot3d_coop_dog.h"

#include "a32_runtime.h"
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_process.h"

#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"

#include <SDL2/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Oot3dNativeGame {

namespace {

constexpr uint32_t kPlayStatePointer = 0x005043D4U + 0x0CU;
constexpr uint32_t kPlayMode = 0x100U;
constexpr uint32_t kPlaySubMode = 0x101U;
constexpr uint32_t kPlaySceneId = 0x104U;
constexpr uint32_t kPlayPlayer = 0x20ACU;
constexpr uint32_t kActorContext = 0x208CU;
constexpr uint32_t kObjectContext = 0x3A58U;
constexpr uint32_t kActorListOffset = 0x0CU;
constexpr uint32_t kActorListCount = 12U;
constexpr uint32_t kActorNext = 0x130U;
constexpr uint32_t kActorUpdate = 0x13CU;
constexpr uint32_t kActorDraw = 0x140U;
constexpr uint32_t kActorWorldPos = 0x28U;
constexpr uint32_t kActorWorldRot = 0x34U;
constexpr uint32_t kActorHomePos = 0x08U;
constexpr uint32_t kActorVelocity = 0x60U;
constexpr uint32_t kActorSpeedXZ = 0x6CU;
constexpr uint32_t kActorGravity = 0x70U;
constexpr uint32_t kActorFloorHeight = 0x84U;
constexpr uint32_t kActorBgCheckFlags = 0x90U;
constexpr uint32_t kActorColChkInfo = 0xA0U;
constexpr uint32_t kActorHealth = kActorColChkInfo + 0x17U;
constexpr uint32_t kActorShape = 0xBCU;
constexpr uint32_t kActorShapeYaw = kActorShape + 0x02U;
constexpr uint32_t kActorPrevPos = 0x108U;
constexpr uint32_t kActorDestroyState = 0x198U;
constexpr uint32_t kActorId = 0x00U;
constexpr uint32_t kActorParams = 0x1CU;
constexpr uint32_t kActorNoop = 0x00161640U;
constexpr uint32_t kActorSpawn = 0x003738D0U;
constexpr uint32_t kActorSpawnTransitionActors = 0x0032525CU;
constexpr uint32_t kObjectSpawn = 0x0032E21CU;
constexpr uint32_t kObjectGetIndex = 0x00363C10U;
constexpr uint32_t kActorBgCheck = 0x00376340U;
constexpr uint32_t kAudioPlayActorSound2 = 0x00375BCCU;
constexpr uint32_t kDirectCallReturn = 0x60000000U;
constexpr uint16_t kActorEnDog = 0x019BU;
constexpr uint16_t kObjectDog = 0x016BU;
constexpr uint16_t kDogBarkSound = 0x28D8U;

float ReadFloat(NativeA32Memory& memory, uint32_t address) {
  uint32_t bits = 0U;
  if (!memory.Read32(address, &bits)) return 0.0F;
  return std::bit_cast<float>(bits);
}

bool WriteFloat(NativeA32Memory& memory, uint32_t address, float value) {
  return memory.Write32(address, std::bit_cast<uint32_t>(value));
}

int16_t ReadS16(NativeA32Memory& memory, uint32_t address) {
  uint16_t value = 0U;
  if (!memory.Read16(address, &value)) return 0;
  return static_cast<int16_t>(value);
}

uint8_t ReadU8(NativeA32Memory& memory, uint32_t address) {
  uint8_t value = 0U;
  if (!memory.Read8(address, &value)) return 0U;
  return value;
}

std::string Lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

int ButtonForName(const std::string& value) {
  std::string name;
  for (const char character : Lowercase(value)) {
    if (character != ' ' && character != '_' && character != '-') {
      name.push_back(character);
    }
  }
  if (name == "a") return SDL_CONTROLLER_BUTTON_A;
  if (name == "b") return SDL_CONTROLLER_BUTTON_B;
  if (name == "x") return SDL_CONTROLLER_BUTTON_X;
  if (name == "y") return SDL_CONTROLLER_BUTTON_Y;
  if (name == "back" || name == "select" || name == "minus") return SDL_CONTROLLER_BUTTON_BACK;
  if (name == "start" || name == "plus") return SDL_CONTROLLER_BUTTON_START;
  if (name == "guide" || name == "home") return SDL_CONTROLLER_BUTTON_GUIDE;
  if (name == "leftshoulder" || name == "l1") return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
  if (name == "rightshoulder" || name == "r1") return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
  if (name == "leftstick" || name == "l3") return SDL_CONTROLLER_BUTTON_LEFTSTICK;
  if (name == "rightstick" || name == "r3") return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
  if (name == "dpadup" || name == "up") return SDL_CONTROLLER_BUTTON_DPAD_UP;
  if (name == "dpaddown" || name == "down") return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  if (name == "dpadleft" || name == "left") return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  if (name == "dpadright" || name == "right") return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  return -1;
}

float ApplyDeadzone(float value, float deadzone) {
  const float magnitude = std::fabs(value);
  if (magnitude <= deadzone) return 0.0F;
  const float scaled = (magnitude - deadzone) / std::max(0.001F, 1.0F - deadzone);
  return std::copysign(std::min(1.0F, scaled), value);
}

bool ButtonHeld(SDL_GameController* controller, int button) {
  return controller != nullptr && button >= 0 &&
         SDL_GameControllerGetButton(controller,
                                     static_cast<SDL_GameControllerButton>(button)) != 0;
}

bool ActionHeld(SDL_GameController* controller, const std::string& value) {
  std::string name;
  for (const char character : Lowercase(value)) {
    if (character != ' ' && character != '_' && character != '-') {
      name.push_back(character);
    }
  }
  if (name == "zl" || name == "lefttrigger" || name == "lt" || name == "l2") {
    return controller != nullptr &&
           SDL_GameControllerGetAxis(controller,
                                     SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192;
  }
  if (name == "zr" || name == "righttrigger" || name == "rt" || name == "r2") {
    return controller != nullptr &&
           SDL_GameControllerGetAxis(controller,
                                     SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192;
  }
  return ButtonHeld(controller, ButtonForName(value));
}

} // namespace

bool CoopDogRuntime::LoadConfig(const std::filesystem::path& path,
                                std::string* error) {
  // Test-only entrance override, used to reach a scene with an existing dog
  // while the universal spawn path is being validated. It is never set by the
  // normal launch profile.
  if (const char* warp = std::getenv("TRIAEVUM_COOP_WARP");
      warp != nullptr && warp[0] != '\0') {
    char* end = nullptr;
    const unsigned long entrance = std::strtoul(warp, &end, 0);
    if (end != warp && entrance <= 0xFFFFUL) {
      testWarpEntrance_ = static_cast<uint16_t>(entrance);
      Log("test warp entrance 0x" + [&] {
        char text[8]{};
        std::snprintf(text, sizeof(text), "%04X",
                      static_cast<uint32_t>(testWarpEntrance_));
        return std::string(text);
      }());
    }
  }
  if (path.empty()) return true;
  std::error_code filesystemError;
  if (!std::filesystem::exists(path, filesystemError)) {
    WriteDefaultConfig(path);
    return true;
  }
  return ParseConfig(path, error);
}

void CoopDogRuntime::WriteDefaultConfig(const std::filesystem::path& path) const {
  if (path.empty()) return;
  std::error_code filesystemError;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), filesystemError);
  }
  nlohmann::json document{
      {"schema_version", 1},
      {"enabled", config_.Enabled},
      {"controller_port", config_.ControllerPort},
      {"movement_stick", config_.MovementStick},
      {"deadzone", config_.Deadzone},
      {"invert_y", config_.InvertY},
      {"jump_button", config_.JumpButton},
      {"bark_button", config_.BarkButton},
      {"respawn_button", config_.RespawnButton},
      {"summon_button", config_.SummonButton},
      {"sprint_button", config_.SprintButton},
      {"move_speed", config_.MoveSpeed},
      {"sprint_speed", config_.SprintSpeed},
      {"bark_range", config_.BarkRange},
      {"bark_damage", config_.BarkDamage},
      {"rupee_pickup_range", config_.RupeePickupRange},
      {"acceleration", config_.Acceleration},
      {"deceleration", config_.Deceleration},
      {"turn_speed", config_.TurnSpeed},
      {"camera_forward_sign", config_.CameraForwardSign},
      {"camera_strafe_sign", config_.CameraStrafeSign},
      {"jump_velocity", config_.JumpVelocity},
      {"gravity", config_.Gravity},
      {"respawn_offset", config_.RespawnOffset},
      {"auto_spawn_on_scene_change", config_.AutoSpawnOnSceneChange},
      {"spawn_if_missing", config_.SpawnIfMissing},
      {"invincible", config_.Invincible},
      {"dog_params", config_.DogParams},
      {"log", config_.Log},
  };
  std::ofstream stream(path);
  if (stream) stream << document.dump(2) << '\n';
}

bool CoopDogRuntime::ParseConfig(const std::filesystem::path& path,
                                 std::string* error) {
  std::ifstream stream(path);
  if (!stream) {
    if (error != nullptr) *error = "cannot open co-op config: " + path.string();
    return false;
  }
  nlohmann::json document;
  try {
    stream >> document;
  } catch (const std::exception& exception) {
    if (error != nullptr) {
      *error = "invalid co-op config " + path.string() + ": " + exception.what();
    }
    return false;
  }
  if (!document.is_object()) {
    if (error != nullptr) *error = "co-op config is not a JSON object: " + path.string();
    return false;
  }
  try {
    config_.Enabled = document.value("enabled", config_.Enabled);
    const int port = document.value("controller_port", static_cast<int>(config_.ControllerPort));
    config_.ControllerPort = static_cast<uint8_t>(std::clamp(port, 0, 7));
    config_.MovementStick = Lowercase(document.value("movement_stick", config_.MovementStick));
    if (config_.MovementStick != "right") config_.MovementStick = "left";
    config_.Deadzone = std::clamp(document.value("deadzone", config_.Deadzone), 0.0F, 0.95F);
    config_.InvertY = document.value("invert_y", config_.InvertY);
    config_.JumpButton = document.value("jump_button", config_.JumpButton);
    config_.BarkButton = document.value("bark_button", config_.BarkButton);
    config_.RespawnButton = document.value("respawn_button", config_.RespawnButton);
    config_.SummonButton = document.value("summon_button", config_.SummonButton);
    config_.SprintButton = document.value("sprint_button", config_.SprintButton);
    config_.MoveSpeed = std::max(0.0F, document.value("move_speed", config_.MoveSpeed));
    config_.SprintSpeed = std::max(config_.MoveSpeed,
                                   document.value("sprint_speed", config_.SprintSpeed));
    config_.BarkRange = std::max(0.0F, document.value("bark_range", config_.BarkRange));
    config_.BarkDamage = std::clamp(document.value("bark_damage", config_.BarkDamage), 0, 255);
    config_.RupeePickupRange = std::max(0.0F, document.value("rupee_pickup_range", config_.RupeePickupRange));
    config_.Acceleration = std::max(0.0F, document.value("acceleration", config_.Acceleration));
    config_.Deceleration = std::max(0.0F, document.value("deceleration", config_.Deceleration));
    config_.TurnSpeed = std::max(0.0F, document.value("turn_speed", config_.TurnSpeed));
    config_.CameraForwardSign = document.value("camera_forward_sign", config_.CameraForwardSign) < 0.0F ? -1.0F : 1.0F;
    config_.CameraStrafeSign = document.value("camera_strafe_sign", config_.CameraStrafeSign) < 0.0F ? -1.0F : 1.0F;
    config_.JumpVelocity = std::max(0.0F, document.value("jump_velocity", config_.JumpVelocity));
    config_.Gravity = std::max(0.0F, document.value("gravity", config_.Gravity));
    config_.RespawnOffset = std::max(0.0F, document.value("respawn_offset", config_.RespawnOffset));
    config_.AutoSpawnOnSceneChange =
        document.value("auto_spawn_on_scene_change", config_.AutoSpawnOnSceneChange);
    config_.SpawnIfMissing = document.value("spawn_if_missing", config_.SpawnIfMissing);
    config_.Invincible = document.value("invincible", config_.Invincible);
    const int params = document.value("dog_params", static_cast<int>(config_.DogParams));
    config_.DogParams = static_cast<uint16_t>(std::clamp(params, 0, 0xFFFF));
    config_.Log = document.value("log", config_.Log);
  } catch (const std::exception& exception) {
    if (error != nullptr) {
      *error = "invalid co-op config value in " + path.string() + ": " + exception.what();
    }
    return false;
  }
  return true;
}

void CoopDogRuntime::PollInput() {
  input_ = {};
  if (!config_.Enabled) return;

  Ship::Context* context = Ship::Context::GetRawInstance();
  if (context == nullptr) return;
  const std::shared_ptr<Ship::ControlDeck> controlDeck = context->GetControlDeck();
  if (!controlDeck) return;
  const auto manager = controlDeck->GetConnectedPhysicalDeviceManager();
  if (!manager) return;

  // The desktop control deck ignores newly attached pads for ports 1..3 until
  // the user assigns them in the controller settings.  A co-op companion is
  // meant to use its own pad, so reserve the second real pad for P2 and keep
  // Steam's virtual mirrors of the same hardware away from both ports.
  {
    std::vector<std::pair<int32_t, std::string>> allPads;
    for (const auto& [instanceId, name] : manager->GetConnectedSDLGamepadNames()) {
      allPads.emplace_back(instanceId, name);
    }
    std::sort(allPads.begin(), allPads.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });
    std::vector<int32_t> physical;
    std::vector<int32_t> virtualPads;
    for (const auto& [instanceId, name] : allPads) {
      const std::string lower = Lowercase(name);
      if (lower.find("steam virtual") != std::string::npos ||
          lower.find("steam controller") != std::string::npos) {
        virtualPads.push_back(instanceId);
      } else {
        physical.push_back(instanceId);
      }
    }
    if (physical.size() >= 2U) {
      // P1 gets the first real pad; P2 gets the second.  Every other device
      // (including Steam's virtual mirrors) is kept away from both ports.
      const int32_t p1 = physical[0];
      const int32_t p2 = physical[1];
      for (const auto& [instanceId, name] : allPads) {
        (void)name;
        manager->IgnoreInstanceIdForPort(0, instanceId);
        manager->IgnoreInstanceIdForPort(config_.ControllerPort, instanceId);
      }
      manager->UnignoreInstanceIdForPort(0, p1);
      manager->UnignoreInstanceIdForPort(config_.ControllerPort, p2);
    }
  }

  std::vector<std::pair<int32_t, SDL_GameController*>> candidates;
  for (const auto& [instanceId, controller] :
       manager->GetConnectedSDLGamepadsForPort(config_.ControllerPort)) {
    if (controller != nullptr &&
        SDL_GameControllerGetAttached(controller) == SDL_TRUE) {
      candidates.emplace_back(instanceId, controller);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  SDL_GameController* controller = nullptr;
  int32_t instanceId = -1;
  for (const auto& candidate : candidates) {
    if (candidate.first == controllerInstance_) {
      controller = candidate.second;
      instanceId = candidate.first;
      break;
    }
  }
  if (controller == nullptr && !candidates.empty()) {
    controller = candidates.front().second;
    instanceId = candidates.front().first;
  }
  if (controller == nullptr) {
    if (!loggedNoController_ && config_.Log) {
      Log("waiting for a second controller on port " +
          std::to_string(static_cast<uint32_t>(config_.ControllerPort)));
      loggedNoController_ = true;
    }
    jumpHeldPrevious_ = false;
    barkHeldPrevious_ = false;
    respawnHeldPrevious_ = false;
    return;
  }
  loggedNoController_ = false;
  if (instanceId != controllerInstance_) {
    jumpHeldPrevious_ = false;
    barkHeldPrevious_ = false;
    respawnHeldPrevious_ = false;
    controllerInstance_ = instanceId;
    if (config_.Log) {
      const char* name = SDL_GameControllerName(controller);
      Log("P2 controller: " + std::string(name != nullptr ? name : "unknown") +
          " (port " + std::to_string(static_cast<uint32_t>(config_.ControllerPort)) + ")");
    }
  }

  const bool rightStick = config_.MovementStick == "right";
  const int16_t rawX = SDL_GameControllerGetAxis(
      controller, rightStick ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
  const int16_t rawY = SDL_GameControllerGetAxis(
      controller, rightStick ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
  input_.MoveX = ApplyDeadzone(static_cast<float>(rawX) / 32767.0F, config_.Deadzone);
  input_.MoveY = ApplyDeadzone(static_cast<float>(rawY) / 32767.0F, config_.Deadzone);
  // SDL reports stick Y down-positive; the dog's forward axis is up-positive.
  input_.MoveY = -input_.MoveY;
  if (config_.InvertY) input_.MoveY = -input_.MoveY;
  input_.Connected = true;

  const bool jump = ButtonHeld(controller, ButtonForName(config_.JumpButton));
  const bool bark = ButtonHeld(controller, ButtonForName(config_.BarkButton));
  const bool respawn = ButtonHeld(controller, ButtonForName(config_.RespawnButton));
  const bool summon = ButtonHeld(controller, ButtonForName(config_.SummonButton));
  input_.Sprinting = ActionHeld(controller, config_.SprintButton);
  if (jump && !jumpHeldPrevious_) jumpLatch_ = true;
  if (bark && !barkHeldPrevious_) barkLatch_ = true;
  if (respawn && !respawnHeldPrevious_) {
    respawnLatch_ = true;
    if (config_.Log) Log("P2 Start pressed");
  }
  if (summon && !summonHeldPrevious_) {
    summonLatch_ = true;
    if (config_.Log) Log("P2 summon pressed");
  }
  jumpHeldPrevious_ = jump;
  barkHeldPrevious_ = bark;
  respawnHeldPrevious_ = respawn;
  summonHeldPrevious_ = summon;
}

void CoopDogRuntime::ObserveBlockEntry(
    uint32_t pc, oot3d::recomp::a32::GuestState& state,
    NativeA32Memory& memory) {
  if (!config_.Enabled) return;
  if (pc == kCoopEnDogDraw) {
    if (hasDesiredState_ && dogActor_ != 0U && state.r[0] == dogActor_) {
      ApplyDesiredState(memory, dogActor_);
    }
    return;
  }
  if (pc == kCoopEnDogUpdate && dogActor_ != 0U &&
      state.r[0] == dogActor_) {
    const float magnitude =
        std::sqrt(input_.MoveX * input_.MoveX + input_.MoveY * input_.MoveY);
    uint16_t nextBehavior = 3U;
    if (barkLatch_) {
      nextBehavior = 2U;
    } else if (magnitude > 0.05F) {
      nextBehavior = 0U;
    }
    const bool hasDesired = hasDesiredState_;
    if (hasDesired) {
      memory.Write16(dogActor_ + kCoopEnDogNextBehavior, nextBehavior);
    }
    if (config_.Log && !enDogUpdateProbeLogged_) {
      enDogUpdateProbeLogged_ = true;
      uint16_t behavior = 0U;
      uint16_t currentNext = 0U;
      memory.Read16(dogActor_ + kCoopEnDogBehavior, &behavior);
      memory.Read16(dogActor_ + kCoopEnDogNextBehavior, &currentNext);
      Log("EnDog_Update entry behavior=" + std::to_string(behavior) +
          " next=" + std::to_string(currentNext) +
          " desired=" + std::to_string(nextBehavior));
    }
    return;
  }
  if (pc != kCoopSceneExecuteCommands || !config_.SpawnIfMissing) return;

  // The room command stream is at r2; object-list entries are relative to
  // that same base.  Copy each list to a private guest page with OBJECT_DOG
  // appended and redirect the command there.  The actor list and every other
  // room payload stay untouched.
  if (objectListScratch_ == 0U) {
    constexpr uint32_t kScratchBase = 0x06300000U;
    constexpr size_t kScratchSize = 0x2000U;
    static const std::array<uint8_t, kScratchSize> zeros{};
    NativeA32MemoryRegionConfig region;
    region.Name = "coop-object-list";
    region.BaseAddress = kScratchBase;
    region.Size = kScratchSize;
    region.Writable = true;
    region.Executable = false;
    region.InitialBytes = zeros;
    std::string error;
    if (!memory.MapRegion(region, &error)) {
      if (config_.Log) Log("cannot map co-op object list: " + error);
      return;
    }
    objectListScratch_ = kScratchBase;
    if (config_.Log) Log("mapped co-op object list scratch");
  }

  const uint32_t commands = state.r[2];
  uint32_t scratchCursor = objectListScratch_;
  for (uint32_t index = 0U; index < 32U; ++index) {
    uint32_t word = 0U;
    uint32_t argument = 0U;
    if (!memory.Read32(commands + index * 8U, &word) ||
        !memory.Read32(commands + index * 8U + 4U, &argument)) {
      return;
    }
    const uint32_t command = word & 0xFFU;
    if (command == 0x14U) return;
    if (command != 0x0BU) continue;
    const uint32_t count = (word >> 8U) & 0xFFU;
    if (count == 0U || count >= 255U) return;
    const uint32_t objectList = commands + argument;
    bool alreadyPresent = false;
    for (uint32_t entry = 0U; entry < count; ++entry) {
      uint16_t id = 0U;
      if (!memory.Read16(objectList + entry * 2U, &id)) return;
      if (id == kObjectDog) alreadyPresent = true;
    }
    if (alreadyPresent) continue;
    if (scratchCursor + (count + 1U) * 2U > objectListScratch_ + 0x2000U) {
      return;
    }
    for (uint32_t entry = 0U; entry < count; ++entry) {
      uint16_t id = 0U;
      if (!memory.Read16(objectList + entry * 2U, &id) ||
          !memory.Write16(scratchCursor + entry * 2U, id)) {
        return;
      }
    }
    if (!memory.Write16(scratchCursor + count * 2U, kObjectDog)) return;
    const uint32_t newArgument = scratchCursor - commands;
    if (!memory.Write32(commands + index * 8U + 4U, newArgument)) return;
    const uint32_t extended = (word & ~0xFF00U) | ((count + 1U) << 8U);
    if (!memory.Write32(commands + index * 8U, extended)) return;
    if (config_.Log) {
      char text[160]{};
      std::snprintf(text, sizeof(text),
                    "redirected room object list to include OBJECT_DOG "
                    "(count %u -> %u)", count, count + 1U);
      Log(text);
    }
    scratchCursor += 0x100U;
  }
}

bool CoopDogRuntime::ObserveNativeBlock(
    uint32_t pc, oot3d::recomp::a32::GuestState& state,
    NativeA32Memory& memory, oot3d::recomp::a32::ExecutionResult* result,
    uint32_t* blocksConsumed) {
  (void)pc;
  (void)state;
  (void)memory;
  (void)result;
  (void)blocksConsumed;
  return false;
}

void CoopDogRuntime::BeforeGuest(NativeA32Process& process) {
  if (!config_.Enabled) return;

  auto& memory = process.Memory();
  uint32_t play = 0U;
  if (!memory.Read32(kPlayStatePointer, &play) || play == 0U) return;
  uint8_t mode = 0U;
  if (!memory.Read8(play + kPlayMode, &mode) || mode != 3U) return;

  if (play != lastPlay_) {
    lastPlay_ = play;
    dogActor_ = 0U;
    sceneSpawnPending_ = config_.AutoSpawnOnSceneChange;
    spawnCooldown_ = 0U;
    hasGameplayFrame_ = false;
    cameraProbeLogged_ = false;
    currentVelocityX_ = currentVelocityZ_ = 0.0F;
  }
  uint16_t scene = 0U;
  if (memory.Read16(play + kPlaySceneId, &scene) && scene != lastScene_) {
    lastScene_ = scene;
    dogActor_ = 0U;
    sceneSpawnPending_ = config_.AutoSpawnOnSceneChange;
    spawnCooldown_ = 0U;
    hasGameplayFrame_ = false;
    cameraProbeLogged_ = false;
    currentVelocityX_ = currentVelocityZ_ = 0.0F;
  }

  // Play mode 3 is the gameplay state; the sub-mode also changes in
  // interiors and during harmless idle transitions, so do not gate the
  // companion's controls on the exterior sub-mode.
  const bool canMove = true;

  if (testWarpEntrance_ != 0U && !testWarpRequested_ && canMove) {
    testWarpRequested_ = true;
    RequestTestWarp(process, play, testWarpEntrance_);
  }

  if (dogActor_ != 0U && !IsDog(process, dogActor_)) {
    dogActor_ = 0U;
  }
  if (dogActor_ == 0U) {
    dogActor_ = FindDog(process, play);
    if (dogActor_ != 0U && config_.Log) {
      uint16_t params = 0U;
      memory.Read16(dogActor_ + kActorParams, &params);
      char actorText[16]{};
      char paramsText[8]{};
      std::snprintf(actorText, sizeof(actorText), "%08X", dogActor_);
      std::snprintf(paramsText, sizeof(paramsText), "%04X",
                    static_cast<uint32_t>(params));
      Log("using dog actor 0x" + std::string(actorText) + " params=0x" +
          std::string(paramsText));
    }
  }
  if (dogActor_ != 0U) {
    sceneSpawnPending_ = false;
  } else {
    const bool wantsSpawn =
        respawnLatch_ ||
        (sceneSpawnPending_ && config_.SpawnIfMissing &&
         config_.AutoSpawnOnSceneChange);
    if (wantsSpawn) {
      if (spawnCooldown_ > 0U) {
        --spawnCooldown_;
      } else if (SpawnDog(process, play)) {
        dogActor_ = FindDog(process, play);
        if (dogActor_ != 0U) {
          sceneSpawnPending_ = false;
          // Do not also teleport/animate in the same frame as Actor_Spawn:
          // the actor model is initialized by the next guest draw.
          respawnLatch_ = false;
          summonLatch_ = false;
          jumpLatch_ = false;
          barkLatch_ = false;
          return;
        }
        spawnCooldown_ = 90U;
      } else {
        spawnCooldown_ = 90U;
      }
    }
    if (dogActor_ == 0U) {
      jumpLatch_ = false;
      barkLatch_ = false;
      // Keep Start latched until the object/scene is ready, so the first
      // summon cannot be lost to a scene-load timing window.
      summonLatch_ = false;
      return;
    }
  }

  if (respawnLatch_ || summonLatch_) {
    if (config_.Log) Log("handling P2 summon/respawn");
    TeleportDog(process, play, dogActor_);
    respawnLatch_ = false;
    summonLatch_ = false;
    jumpLatch_ = false;
  } else {
    UpdateDog(process, play, dogActor_, canMove);
  }
  if (barkLatch_) {
    CallBark(process, play, dogActor_);
    barkLatch_ = false;
  }
  jumpLatch_ = false;
}

uint32_t CoopDogRuntime::FindDog(NativeA32Process& process, uint32_t play) const {
  auto& memory = process.Memory();
  uint32_t first = 0U;
  uint32_t exact = 0U;
  uint32_t colour = 0U;
  const uint16_t wantedParams = config_.DogParams;
  for (uint32_t list = 0U; list < kActorListCount; ++list) {
    uint32_t head = 0U;
    if (!memory.Read32(play + kActorContext + kActorListOffset + list * 8U + 4U,
                       &head)) {
      continue;
    }
    uint32_t actor = head;
    for (uint32_t index = 0U; actor != 0U && index < 512U; ++index) {
      uint16_t id = 0U;
      if (!memory.Read16(actor + kActorId, &id)) break;
      if (id == kActorEnDog) {
        uint16_t params = 0U;
        memory.Read16(actor + kActorParams, &params);
        if (first == 0U) first = actor;
        if (wantedParams != 0U && params == wantedParams && exact == 0U) {
          exact = actor;
        }
        if (wantedParams != 0U &&
            (params & 0x0F00U) == (wantedParams & 0x0F00U) &&
            colour == 0U) {
          colour = actor;
        }
      }
      uint32_t next = 0U;
      if (!memory.Read32(actor + kActorNext, &next)) break;
      actor = next;
    }
  }
  if (exact != 0U) return exact;
  if (colour != 0U) return colour;
  return first;
}

bool CoopDogRuntime::IsDog(NativeA32Process& process, uint32_t actor) const {
  if (actor == 0U) return false;
  auto& memory = process.Memory();
  uint16_t id = 0U;
  uint32_t draw = 0U;
  return memory.Read16(actor + kActorId, &id) && id == kActorEnDog &&
         memory.Read32(actor + kActorDraw, &draw) && draw != 0U;
}

bool CoopDogRuntime::SpawnDog(NativeA32Process& process, uint32_t play) {
  if (!CallObjectSpawn(process, play)) return false;

  uint32_t player = 0U;
  process.Memory().Read32(play + kPlayPlayer, &player);
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  int16_t yaw = 0;
  if (player != 0U) {
    x = ReadFloat(process.Memory(), player + kActorWorldPos);
    y = ReadFloat(process.Memory(), player + kActorWorldPos + 4U);
    z = ReadFloat(process.Memory(), player + kActorWorldPos + 8U);
    yaw = ReadS16(process.Memory(), player + kActorWorldRot + 2U);
  }
  const float offset = config_.RespawnOffset;
  const float sideX = std::cos(static_cast<float>(yaw) * 3.14159265358979323846F / 32768.0F);
  const float sideZ = -std::sin(static_cast<float>(yaw) * 3.14159265358979323846F / 32768.0F);
  x += sideX * offset;
  z += sideZ * offset;
  y += 10.0F;

  uint32_t actor = 0U;
  if (!CallActorSpawn(process, play, x, y, z, &actor)) return false;
  if (actor == 0U && config_.Log) {
    Log("Actor_Spawn(EN_DOG) returned 0");
  }
  if (actor != 0U && IsDog(process, actor)) {
    dogActor_ = actor;
  } else {
    dogActor_ = FindDog(process, play);
  }
  if (dogActor_ == 0U) {
    Log("Actor_Spawn returned no EN_DOG actor");
    return false;
  }
  process.Memory().Write32(dogActor_ + kActorUpdate, kActorNoop);
  desiredAnimation_ = 0xFFFFFFFFU;
  desiredX_ = ReadFloat(process.Memory(), dogActor_ + kActorWorldPos);
  desiredY_ = ReadFloat(process.Memory(), dogActor_ + kActorWorldPos + 4U);
  desiredZ_ = ReadFloat(process.Memory(), dogActor_ + kActorWorldPos + 8U);
  desiredYaw_ = ReadS16(process.Memory(), dogActor_ + kActorWorldRot + 2U);
  hasDesiredState_ = true;
  if (config_.Log) {
    uint16_t params = 0U;
    process.Memory().Read16(dogActor_ + kActorParams, &params);
    Log("spawned co-op dog at 0x" + [&] {
      char text[16]{};
      std::snprintf(text, sizeof(text), "%08X", dogActor_);
      return std::string(text);
    }() + " params=0x" + [&] {
      char text[8]{};
      std::snprintf(text, sizeof(text), "%04X", static_cast<uint32_t>(params));
      return std::string(text);
    }());
  }
  return true;
}

bool CoopDogRuntime::CallObjectSpawn(NativeA32Process& process, uint32_t play) {
  std::string error;
  if (!objectContextProbeLogged_ && config_.Log) {
    objectContextProbeLogged_ = true;
    auto& memory = process.Memory();
    uint8_t count = 0U;
    memory.Read8(play + kObjectContext, &count);
    std::string ids;
    for (uint32_t index = 0U; index < std::min<uint32_t>(count, 19U); ++index) {
      uint16_t id = 0U;
      if (!memory.Read16(play + kObjectContext + 4U + index * 0x80U, &id)) break;
      char item[16]{};
      std::snprintf(item, sizeof(item), " %04X", static_cast<uint32_t>(id));
      ids += item;
    }
    char text[240]{};
    std::snprintf(text, sizeof(text), "object context count=%u ids:%s",
                  static_cast<uint32_t>(count), ids.c_str());
    Log(text);
    const std::array<uint32_t, 2> knownArguments{
        play + kObjectContext, 0x012FU};
    uint32_t knownSlot = 0U;
    if (process.InvokeFunctionWithResult(kObjectGetIndex, knownArguments,
                                         kDirectCallReturn, &knownSlot,
                                         &error)) {
      Log("known OBJECT_012F slot=" + std::to_string(
          static_cast<int32_t>(knownSlot)));
    }
  }
  const std::array<uint32_t, 2> arguments{play + kObjectContext,
                                          static_cast<uint32_t>(kObjectDog)};
  uint32_t slot = 0U;
  if (!process.InvokeFunctionWithResult(kObjectGetIndex, arguments,
                                        kDirectCallReturn, &slot, &error)) {
    Log("Object_GetIndex(OBJECT_DOG) failed: " + error);
    return false;
  }
  if (static_cast<int32_t>(slot) < 0) {
    if (!objectIndexProbeLogged_ && config_.Log) {
      objectIndexProbeLogged_ = true;
      Log("OBJECT_DOG object context is not ready yet");
    }
    return false;
  }
  return true;
}

bool CoopDogRuntime::CallActorSpawn(NativeA32Process& process, uint32_t play,
                                    float x, float y, float z,
                                    uint32_t* actor) {
  if (actor != nullptr) *actor = 0U;
  auto state = process.PrimaryThreadState();
  const uint32_t stack = (state.r[13] - 0x100U) & ~7U;
  auto& memory = process.Memory();
  // AAPCS: r0=actorContext, r1=play, r2=actorId, r3=rotX; x/y/z in s0/s1/s2;
  // rotY/rotZ/params/initializeNow are the four stack words.
  if (!memory.Write32(stack + 0U, 0U) ||
      !memory.Write32(stack + 4U, 0U) ||
      !memory.Write32(stack + 8U, static_cast<uint32_t>(config_.DogParams)) ||
      !memory.Write32(stack + 12U, 1U)) {
    Log("cannot write Actor_Spawn stack arguments");
    return false;
  }
  state.r[0] = play + kActorContext;
  state.r[1] = play;
  state.r[2] = kActorEnDog;
  state.r[3] = 0U;
  state.r[13] = stack;
  state.vfp[0] = std::bit_cast<uint32_t>(x);
  state.vfp[1] = std::bit_cast<uint32_t>(y);
  state.vfp[2] = std::bit_cast<uint32_t>(z);
  state.vfp[3] = 0U;
  std::string error;
  if (!process.InvokeFunctionWithState(kActorSpawn, state, kDirectCallReturn,
                                       &error, 10'000'000U, 100'000U)) {
    Log("Actor_Spawn(EN_DOG) failed: " + error);
    return false;
  }
  if (actor != nullptr) *actor = state.r[0];
  return true;
}

bool CoopDogRuntime::CallActorBgCheck(NativeA32Process& process, uint32_t play,
                                     uint32_t actor) {
  auto state = process.PrimaryThreadState();
  state.r[0] = play;
  state.r[1] = actor;
  state.r[2] = 0x1DU;
  state.r[13] = (state.r[13] - 0x80U) & ~7U;
  state.vfp[0] = std::bit_cast<uint32_t>(20.0F);
  state.vfp[1] = std::bit_cast<uint32_t>(20.0F);
  state.vfp[2] = std::bit_cast<uint32_t>(50.0F);
  state.vfp[3] = 0U;
  std::string error;
  return process.InvokeFunctionWithState(kActorBgCheck, state,
                                         kDirectCallReturn, &error);
}

bool CoopDogRuntime::RequestTestWarp(NativeA32Process& process, uint32_t play,
                                     uint16_t entrance) {
  constexpr uint32_t kRequestTransition = 0x003348E8U;
  constexpr uint32_t kPrepareTransitionEffect = 0x0035DA3CU;
  constexpr uint32_t kTransitionTrigger = 20U;
  const std::array<uint32_t, 3> arguments{
      play, static_cast<uint32_t>(entrance), kTransitionTrigger};
  uint32_t accepted = 0U;
  std::string error;
  if (!process.InvokeFunctionWithResult(kRequestTransition, arguments,
                                        kDirectCallReturn, &accepted,
                                        &error)) {
    Log("test warp request failed: " + error);
    return false;
  }
  if (accepted == 0U) {
    Log("test warp request was not accepted");
    return false;
  }
  const std::array<uint32_t, 1> prepare{play};
  if (!process.InvokeFunction(kPrepareTransitionEffect, prepare,
                              kDirectCallReturn, &error)) {
    Log("test warp effect preparation failed: " + error);
    return false;
  }
  Log("test warp requested");
  return true;
}

void CoopDogRuntime::CallBark(NativeA32Process& process, uint32_t play,
                              uint32_t actor) {
  // The native Audio_PlayActorSound2 entry point requires more guest runtime
  // state than a host-side call provides and faults when invoked this way, so
  // keep the bark visual and the gameplay damage instead of the sound effect.
  ApplyBarkAttack(process, play, actor);
}

void CoopDogRuntime::ApplyBarkAttack(NativeA32Process& process, uint32_t play,
                                     uint32_t dog) {
  auto& memory = process.Memory();
  const float dogX = ReadFloat(memory, dog + kActorWorldPos);
  const float dogY = ReadFloat(memory, dog + kActorWorldPos + 4U);
  const float dogZ = ReadFloat(memory, dog + kActorWorldPos + 8U);
  const int16_t yaw = ReadS16(memory, dog + kActorWorldRot + 2U);
  const float forwardX =
      std::sin(static_cast<float>(yaw) * 6.28318530717958647692F / 65536.0F);
  const float forwardZ =
      std::cos(static_cast<float>(yaw) * 6.28318530717958647692F / 65536.0F);
  uint32_t player = 0U;
  memory.Read32(play + kPlayPlayer, &player);
  uint32_t hitCount = 0U;
  const float range = config_.BarkRange;
  for (uint32_t list = 0U; list < kActorListCount; ++list) {
    uint32_t actor = 0U;
    if (!memory.Read32(play + kActorContext + kActorListOffset + list * 8U + 4U,
                       &actor)) {
      continue;
    }
    for (uint32_t index = 0U; actor != 0U && index < 512U; ++index) {
      uint16_t id = 0U;
      memory.Read16(actor + kActorId, &id);
      if (id != 0U && actor != dog && actor != player) {
        const uint8_t category = ReadU8(memory, actor + 2U);
        if (id != kCoopActorItem00 &&
            (category == 3U || category == 4U || category == 5U ||
             category == 8U)) {
          const float x = ReadFloat(memory, actor + kActorWorldPos);
          const float y = ReadFloat(memory, actor + kActorWorldPos + 4U);
          const float z = ReadFloat(memory, actor + kActorWorldPos + 8U);
          const float dx = x - dogX;
          const float dz = z - dogZ;
          const float distance = std::sqrt(dx * dx + dz * dz);
          const float vertical = std::fabs(y - dogY);
          if (distance <= range && vertical <= range) {
            const float facing =
                distance > 0.001F
                    ? (dx * forwardX + dz * forwardZ) / distance
                    : 1.0F;
            if (facing >= 0.15F) {
              const uint8_t health = ReadU8(memory, actor + kActorHealth);
              if (health != 0U) {
                memory.Write8(actor + 0xB8U,
                              static_cast<uint8_t>(config_.BarkDamage));
                const std::array<uint32_t, 1> arguments{actor};
                std::string damageError;
                process.InvokeFunctionWithResult(
                    kCoopActorApplyDamage, arguments, kDirectCallReturn,
                    nullptr, &damageError);
                if (category == 5U) {
                  process.InvokeFunction(kCoopActorKill, arguments,
                                         kDirectCallReturn, &damageError);
                }
                ++hitCount;
              }
            }
          }
        }
      }
      uint32_t next = 0U;
      if (!memory.Read32(actor + kActorNext, &next)) break;
      actor = next;
    }
  }
  if (config_.Log && hitCount != 0U) {
    Log("dog bark hit " + std::to_string(hitCount) + " target(s)");
  }
}

void CoopDogRuntime::CollectRupees(NativeA32Process& process, uint32_t play,
                                   uint32_t dog) {
  auto& memory = process.Memory();
  const float dogX = ReadFloat(memory, dog + kActorWorldPos);
  const float dogY = ReadFloat(memory, dog + kActorWorldPos + 4U);
  const float dogZ = ReadFloat(memory, dog + kActorWorldPos + 8U);
  for (uint32_t list = 0U; list < kActorListCount; ++list) {
    uint32_t actor = 0U;
    if (!memory.Read32(play + kActorContext + kActorListOffset + list * 8U + 4U,
                       &actor)) {
      continue;
    }
    for (uint32_t index = 0U; actor != 0U && index < 512U; ++index) {
      uint16_t id = 0U;
      memory.Read16(actor + kActorId, &id);
      if (id == kCoopActorItem00) {
        const uint16_t params = static_cast<uint16_t>(ReadS16(
            memory, actor + kActorParams));
        const uint16_t item = params & 0xFFU;
        uint32_t amount = 0U;
        if (item == 0x84U) amount = 1U;
        else if (item == 0x85U) amount = 5U;
        else if (item == 0x86U) amount = 20U;
        else if (item == 0x87U) amount = 50U;
        else if (item == 0x88U) amount = 200U;
        if (amount != 0U) {
          const float x = ReadFloat(memory, actor + kActorWorldPos);
          const float y = ReadFloat(memory, actor + kActorWorldPos + 4U);
          const float z = ReadFloat(memory, actor + kActorWorldPos + 8U);
          const float dx = x - dogX;
          const float dz = z - dogZ;
          if (std::sqrt(dx * dx + dz * dz) <= config_.RupeePickupRange &&
              std::fabs(y - dogY) <= 60.0F) {
            const std::array<uint32_t, 1> amountArguments{amount};
            std::string itemError;
            process.InvokeFunction(kCoopRupeesChangeBy, amountArguments,
                                   kDirectCallReturn, &itemError);
            const std::array<uint32_t, 1> actorArguments{actor};
            process.InvokeFunction(kCoopActorKill, actorArguments,
                                   kDirectCallReturn, &itemError);
            if (config_.Log) {
              Log("dog collected a rupee (" + std::to_string(amount) + ")");
            }
          }
        }
      }
      uint32_t next = 0U;
      if (!memory.Read32(actor + kActorNext, &next)) break;
      actor = next;
    }
  }
}

void CoopDogRuntime::TeleportDog(NativeA32Process& process, uint32_t play,
                                 uint32_t actor) {
  auto& memory = process.Memory();
  uint32_t player = 0U;
  memory.Read32(play + kPlayPlayer, &player);
  float x = ReadFloat(memory, actor + kActorWorldPos);
  float y = ReadFloat(memory, actor + kActorWorldPos + 4U);
  float z = ReadFloat(memory, actor + kActorWorldPos + 8U);
  int16_t yaw = ReadS16(memory, actor + kActorWorldRot + 2U);
  if (player != 0U) {
    x = ReadFloat(memory, player + kActorWorldPos);
    y = ReadFloat(memory, player + kActorWorldPos + 4U) + 10.0F;
    z = ReadFloat(memory, player + kActorWorldPos + 8U);
    yaw = ReadS16(memory, player + kActorWorldRot + 2U);
    const float angle = static_cast<float>(yaw) * 3.14159265358979323846F /
                        32768.0F;
    x += std::cos(angle) * config_.RespawnOffset;
    z -= std::sin(angle) * config_.RespawnOffset;
  }
  WriteFloat(memory, actor + kActorWorldPos, x);
  WriteFloat(memory, actor + kActorWorldPos + 4U, y);
  WriteFloat(memory, actor + kActorWorldPos + 8U, z);
  WriteFloat(memory, actor + kActorHomePos, x);
  WriteFloat(memory, actor + kActorHomePos + 4U, y);
  WriteFloat(memory, actor + kActorHomePos + 8U, z);
  WriteFloat(memory, actor + kActorPrevPos, x);
  WriteFloat(memory, actor + kActorPrevPos + 4U, y);
  WriteFloat(memory, actor + kActorPrevPos + 8U, z);
  WriteFloat(memory, actor + kActorVelocity, 0.0F);
  WriteFloat(memory, actor + kActorVelocity + 4U, 0.0F);
  WriteFloat(memory, actor + kActorVelocity + 8U, 0.0F);
  WriteFloat(memory, actor + kActorSpeedXZ, 0.0F);
  WriteFloat(memory, actor + kActorGravity, -config_.Gravity * config_.TickSeconds);
  memory.Write16(actor + kActorWorldRot + 2U, static_cast<uint16_t>(yaw));
  memory.Write16(actor + kActorShapeYaw, static_cast<uint16_t>(yaw));
  memory.Write16(actor + kActorBgCheckFlags, 0U);
  memory.Write8(actor + kActorHealth, config_.Invincible ? 8U : 4U);
  memory.Write8(actor + kActorDestroyState, 0U);
  memory.Write32(actor + kActorUpdate, kActorNoop);
  desiredAnimation_ = 0xFFFFFFFFU;
  CallActorBgCheck(process, play, actor);
  const float floorHeight = ReadFloat(memory, actor + kActorFloorHeight);
  float finalY = ReadFloat(memory, actor + kActorWorldPos + 4U);
  if (finalY < floorHeight) {
    finalY = floorHeight;
    WriteFloat(memory, actor + kActorWorldPos + 4U, finalY);
  }
  currentVelocityX_ = currentVelocityZ_ = 0.0F;
  desiredX_ = ReadFloat(memory, actor + kActorWorldPos);
  desiredY_ = finalY;
  desiredZ_ = ReadFloat(memory, actor + kActorWorldPos + 8U);
  desiredYaw_ = ReadS16(memory, actor + kActorWorldRot + 2U);
  hasDesiredState_ = true;
  ApplyDesiredState(memory, actor);
  if (config_.Log) {
    Log("respawned co-op dog near Link");
  }
}

float CoopDogRuntime::ReadCameraYaw(NativeA32Memory& memory,
                                    uint32_t play) {
  // The main camera struct is embedded in PlayState at +0x364; it is not a
  // pointer stored at that offset.
  const uint32_t camera = play + 0x364U;
  // The PlayState view is the camera the renderer actually uses: eye at
  // +0x1B8 and at at +0x1C4.  The Camera struct fields at +0x8C/+0x80 are
  // kept as a fallback for states where the view has not been published yet.
  float eyeX = ReadFloat(memory, play + 0x1B8U);
  float eyeZ = ReadFloat(memory, play + 0x1C0U);
  float atX = ReadFloat(memory, play + 0x1C4U);
  float atZ = ReadFloat(memory, play + 0x1CCU);
  const float cameraEyeX = ReadFloat(memory, camera + 0x8CU);
  const float cameraEyeZ = ReadFloat(memory, camera + 0x94U);
  const float cameraAtX = ReadFloat(memory, camera + 0x80U);
  const float cameraAtZ = ReadFloat(memory, camera + 0x88U);
  const float viewDx = atX - eyeX;
  const float viewDz = atZ - eyeZ;
  float result = std::numeric_limits<float>::quiet_NaN();
  if (std::isfinite(viewDx) && std::isfinite(viewDz) &&
      std::fabs(viewDx) + std::fabs(viewDz) >= 0.0001F) {
    result = std::atan2(viewDx, viewDz);
  } else {
    eyeX = cameraEyeX;
    eyeZ = cameraEyeZ;
    atX = cameraAtX;
    atZ = cameraAtZ;
    const float dx = atX - eyeX;
    const float dz = atZ - eyeZ;
    if (std::isfinite(dx) && std::isfinite(dz) &&
        std::fabs(dx) + std::fabs(dz) >= 0.0001F) {
      result = std::atan2(dx, dz);
    } else {
      const int16_t yaw = ReadS16(memory, camera + 0x26U);
      result = static_cast<float>(yaw) * 6.28318530717958647692F / 65536.0F;
    }
  }
  if (config_.Log && !cameraProbeLogged_) {
    cameraProbeLogged_ = true;
    char text[300]{};
    std::snprintf(text, sizeof(text),
                  "view eye=(%.2f,%.2f) at=(%.2f,%.2f) yaw=%.3f; "
                  "camera eye=(%.2f,%.2f) at=(%.2f,%.2f)",
                  eyeX, eyeZ, atX, atZ, result, cameraEyeX, cameraEyeZ,
                  cameraAtX, cameraAtZ);
    Log(text);
  }
  return result;
}

void CoopDogRuntime::ApplyDesiredState(NativeA32Memory& memory,
                                       uint32_t actor) const {
  if (!hasDesiredState_ || actor == 0U) return;
  WriteFloat(memory, actor + kActorWorldPos, desiredX_);
  WriteFloat(memory, actor + kActorWorldPos + 4U, desiredY_);
  WriteFloat(memory, actor + kActorWorldPos + 8U, desiredZ_);
  memory.Write16(actor + kActorWorldRot + 2U,
                 static_cast<uint16_t>(desiredYaw_));
  memory.Write16(actor + kActorShapeYaw, static_cast<uint16_t>(desiredYaw_));
}

void CoopDogRuntime::UpdateDog(NativeA32Process& process, uint32_t play,
                               uint32_t actor, bool canMove) {
  auto& memory = process.Memory();
  // The native AI owns its own movement and often chooses to sit/run away.
  // Disable it; the co-op runtime drives SkelAnime explicitly below.
  memory.Write32(actor + kActorUpdate, kActorNoop);
  if (config_.Invincible) {
    memory.Write8(actor + kActorHealth, 8U);
  }
  // EnDog_Update's native behavior may walk/flee before the draw pass. Keep
  // the actor in the draw list so EnDog_Draw can re-apply the P2 pose.
  WriteFloat(memory, actor + 0x0FCU, 1000000.0F);
  WriteFloat(memory, actor + 0x100U, 1000000.0F);
  WriteFloat(memory, actor + 0x104U, 1000000.0F);

  float x = ReadFloat(memory, actor + kActorWorldPos);
  float y = ReadFloat(memory, actor + kActorWorldPos + 4U);
  float z = ReadFloat(memory, actor + kActorWorldPos + 8U);
  int16_t yaw = ReadS16(memory, actor + kActorWorldRot + 2U);
  if (!canMove) {
    desiredX_ = x;
    desiredY_ = y;
    desiredZ_ = z;
    desiredYaw_ = yaw;
    hasDesiredState_ = true;
    ApplyDesiredState(memory, actor);
    return;
  }

  uint32_t gameplayFrame = 0U;
  const bool haveGameplayFrame =
      memory.Read32(play + 0x5BF4U, &gameplayFrame);
  if (hasGameplayFrame_ && haveGameplayFrame &&
      gameplayFrame == lastGameplayFrame_) {
    if (hasDesiredState_) ApplyDesiredState(memory, actor);
    return;
  }
  float dt = config_.TickSeconds;
  if (hasGameplayFrame_ && haveGameplayFrame &&
      gameplayFrame >= lastGameplayFrame_) {
    const uint32_t frames = gameplayFrame - lastGameplayFrame_;
    if (frames > 0U && frames < 10U) {
      dt = std::clamp(static_cast<float>(frames) / 30.0F,
                      1.0F / 60.0F, 1.0F / 15.0F);
    }
  }
  lastGameplayFrame_ = gameplayFrame;
  hasGameplayFrame_ = haveGameplayFrame;

  float velocityY = ReadFloat(memory, actor + kActorVelocity + 4U);
  const uint16_t bgFlags = [&] {
    uint16_t value = 0U;
    memory.Read16(actor + kActorBgCheckFlags, &value);
    return value;
  }();
  const bool onGround = (bgFlags & 1U) != 0U;

  float cameraYaw = ReadCameraYaw(memory, play);
  if (!std::isfinite(cameraYaw)) {
    cameraYaw = static_cast<float>(yaw) * 6.28318530717958647692F / 65536.0F;
  }
  const float stickX = input_.MoveX * config_.CameraStrafeSign;
  const float stickY = input_.MoveY * config_.CameraForwardSign;
  float directionX = std::sin(cameraYaw) * stickY +
                     std::cos(cameraYaw) * stickX;
  float directionZ = std::cos(cameraYaw) * stickY -
                     std::sin(cameraYaw) * stickX;
  const float magnitude =
      std::sqrt(directionX * directionX + directionZ * directionZ);
  float targetVelocityX = 0.0F;
  float targetVelocityZ = 0.0F;
  if (magnitude > 0.05F) {
    const float normalized = std::min(1.0F, magnitude);
    directionX /= magnitude;
    directionZ /= magnitude;
    const float speed =
        (input_.Sprinting ? config_.SprintSpeed : config_.MoveSpeed) * normalized;
    targetVelocityX = directionX * speed;
    targetVelocityZ = directionZ * speed;
    int16_t targetYaw = static_cast<int16_t>(std::lround(
        std::atan2(directionX, directionZ) * 65536.0F /
        6.28318530717958647692F));
    int32_t delta = static_cast<int32_t>(targetYaw) - yaw;
    delta = ((delta + 0x8000) & 0xFFFF) - 0x8000;
    const int32_t maxStep = static_cast<int32_t>(std::lround(
        config_.TurnSpeed * dt * 65536.0F / 360.0F));
    if (delta > maxStep) delta = maxStep;
    if (delta < -maxStep) delta = -maxStep;
    yaw = static_cast<int16_t>(static_cast<int32_t>(yaw) + delta);
  }

  const auto approach = [](float value, float target, float maxDelta) {
    if (value < target) return std::min(target, value + maxDelta);
    if (value > target) return std::max(target, value - maxDelta);
    return value;
  };
  const bool hasInput = magnitude > 0.05F;
  const float rate = (hasInput ? config_.Acceleration : config_.Deceleration) * dt;
  currentVelocityX_ = approach(currentVelocityX_, targetVelocityX, rate);
  currentVelocityZ_ = approach(currentVelocityZ_, targetVelocityZ, rate);
  if (std::fabs(currentVelocityX_) < 0.01F) currentVelocityX_ = 0.0F;
  if (std::fabs(currentVelocityZ_) < 0.01F) currentVelocityZ_ = 0.0F;
  const float velocityX = currentVelocityX_;
  const float velocityZ = currentVelocityZ_;
  const float horizontalSpeed =
      std::sqrt(velocityX * velocityX + velocityZ * velocityZ);

  // Manual SkelAnime control: the native dog behavior is disabled, so choose
  // the same animation indices the native state machine uses.  Idle keeps the
  // sit loop, movement uses walk, bark uses the bark clip.
  uint32_t animationInfo = 0U;
  memory.Read32(kCoopDogAnimationInfoPointer, &animationInfo);
  uint32_t animation = 4U; // ENDOG_ANIM_4: sit
  if (barkLatch_) {
    animation = 3U; // bark
  } else if (!onGround || jumpLatch_) {
    // The dog object has no dedicated jump clip; use the run pose in the air
    // instead of leaving the sitting animation frozen.
    animation = 2U;
  } else if (hasInput || horizontalSpeed > 1.0F) {
    animation = input_.Sprinting ? 2U : 1U;
  }
  if (animationInfo != 0U && animation != desiredAnimation_) {
    const std::array<uint32_t, 3> arguments{
        actor + kCoopEnDogSkelAnime, animationInfo, animation};
    std::string animationError;
    if (!process.InvokeFunction(kCoopAnimationChangeByInfo, arguments,
                                kDirectCallReturn, &animationError)) {
      if (config_.Log) Log("dog animation change failed: " + animationError);
    } else {
      desiredAnimation_ = animation;
    }
  }
  {
    const std::array<uint32_t, 1> arguments{actor + kCoopEnDogSkelAnime};
    std::string animationError;
    if (!process.InvokeFunction(kCoopSkelAnimeUpdate, arguments,
                                kDirectCallReturn, &animationError) &&
        config_.Log) {
      Log("dog SkelAnime update failed: " + animationError);
    }
  }
  if (jumpLatch_ && onGround) {
    velocityY = config_.JumpVelocity;
  } else {
    velocityY -= config_.Gravity * dt;
  }
  velocityY = std::max(velocityY, -800.0F);

  x += velocityX * dt;
  y += velocityY * dt;
  z += velocityZ * dt;
  WriteFloat(memory, actor + kActorWorldPos, x);
  WriteFloat(memory, actor + kActorWorldPos + 4U, y);
  WriteFloat(memory, actor + kActorWorldPos + 8U, z);
  WriteFloat(memory, actor + kActorVelocity, velocityX);
  WriteFloat(memory, actor + kActorVelocity + 4U, velocityY);
  WriteFloat(memory, actor + kActorVelocity + 8U, velocityZ);
  WriteFloat(memory, actor + kActorSpeedXZ,
             std::sqrt(velocityX * velocityX + velocityZ * velocityZ));
  WriteFloat(memory, actor + kActorGravity, -config_.Gravity * dt);
  memory.Write16(actor + kActorWorldRot + 2U, static_cast<uint16_t>(yaw));
  memory.Write16(actor + kActorShapeYaw, static_cast<uint16_t>(yaw));
  memory.Write8(actor + kActorHealth, config_.Invincible ? 8U : 4U);

  CallActorBgCheck(process, play, actor);
  uint16_t afterFlags = 0U;
  memory.Read16(actor + kActorBgCheckFlags, &afterFlags);
  const float floorHeight = ReadFloat(memory, actor + kActorFloorHeight);
  float finalY = ReadFloat(memory, actor + kActorWorldPos + 4U);
  if (((afterFlags & 1U) != 0U && velocityY <= 0.0F) || finalY < floorHeight) {
    finalY = floorHeight;
  }
  WriteFloat(memory, actor + kActorWorldPos + 4U, finalY);
  if (((afterFlags & 1U) != 0U && velocityY <= 0.0F) || finalY <= floorHeight) {
    velocityY = 0.0F;
  }
  WriteFloat(memory, actor + kActorVelocity + 4U, velocityY);
  desiredX_ = x;
  desiredY_ = finalY;
  desiredZ_ = z;
  desiredYaw_ = yaw;
  hasDesiredState_ = true;
  ApplyDesiredState(memory, actor);
  CollectRupees(process, play, actor);
}

void CoopDogRuntime::Log(const std::string& message) const {
  if (!config_.Log) return;
  std::cout << "triaevum coop: " << message << '\n';
}

} // namespace Oot3dNativeGame
