#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Oot3dNativeGame {

struct NetplayPose {
  std::uint32_t Sequence = 0;
  std::array<float, 3> Position{};
  float Yaw = 0.0F;
  std::uint32_t Animation = 0;
  float AnimationFrame = 0.0F;
  float AnimationSpeed = 0.0F;
  float AnimationStart = 0.0F;
  float AnimationEnd = 0.0F;
  std::uint16_t SceneId = 0;
  std::uint16_t RoomId = 0;
  std::uint16_t DayTime = 0;
  std::uint16_t Health = 0;
  std::uint16_t Wallet = 0;
  std::int32_t EntranceId = 0;
  std::uint64_t SwitchFlags = 0;
  std::uint64_t CollectibleFlags = 0;
};

struct NetplayScreenPoint {
  bool Visible = false;
  float X = 0.0F;
  float Y = 0.0F;
};

// Projects a remote world position through the local scene camera. Kept pure
// so the M2 proxy visibility can be tested without a running guest.
// The remote world clock and player resources are only meaningful once both
// sessions have entered the same scene. Scene 0 is the pre-play/title state.
bool CanApplyRemoteWorld(const NetplayPose& local, const NetplayPose& remote);

// Converts the network's radians to the guest's s16 binary angle. Kept pure so
// the remote-actor transform and the packet tests share one definition.
std::int16_t NetplayYawToBinaryAngle(float radians);

NetplayScreenPoint ProjectNetplayPoint(
    const NetplayPose& pose, const std::array<float, 3>& eye,
    const std::array<float, 3>& at, float left, float right, float bottom,
    float top, float nearPlane, std::uint32_t width, std::uint32_t height);

struct NetplayOptions {
  bool Enabled = false;
  bool Host = false;
  std::string Address = "127.0.0.1";
  std::uint16_t Port = 27888;
  std::uint8_t Player = 1;
};

// Minimal loopback/LAN transport for milestone M1. It exchanges input-free
// player pose snapshots over UDP. There is no world replication or reliability
// layer yet; movement is deliberately unreliable and events will be added in a
// later milestone.
class NetplaySession final {
public:
  NetplaySession() = default;
  ~NetplaySession();

  NetplaySession(const NetplaySession&) = delete;
  NetplaySession& operator=(const NetplaySession&) = delete;

  bool Start(const NetplayOptions& options, std::string* error = nullptr);
  void Stop();
  [[nodiscard]] bool Enabled() const noexcept { return mEnabled; }
  [[nodiscard]] bool Connected() const noexcept;
  [[nodiscard]] std::uint8_t LocalPlayer() const noexcept { return mOptions.Player; }
  [[nodiscard]] const NetplayOptions& Options() const noexcept { return mOptions; }

  // Called once per host frame. Sends the local pose at a bounded rate and
  // consumes all pending datagrams.
  void Poll(double seconds, const NetplayPose& local);
  [[nodiscard]] bool LatestRemotePose(NetplayPose* pose) const noexcept;

private:
  enum class PacketType : std::uint8_t { Hello = 1, HelloAck = 2, State = 3, Bye = 4 };

  void SendPacket(PacketType type, const NetplayPose& pose);
  void ConsumePacket(const std::uint8_t* bytes, std::size_t size);

  NetplayOptions mOptions;
  int mSocket = -1;
  bool mEnabled = false;
  bool mPeerKnown = false;
  bool mAcknowledged = false;
  std::array<std::uint8_t, 4> mPeerAddress{};
  std::uint16_t mPeerPort = 0;
  std::uint32_t mSequence = 0;
  double mSendAccumulator = 0.0;
  double mLastReceiveSeconds = 0.0;
  double mElapsedSeconds = 0.0;
  NetplayPose mRemote;
  bool mHasRemote = false;
};

} // namespace Oot3dNativeGame
