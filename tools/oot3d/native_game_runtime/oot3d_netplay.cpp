#include "oot3d_netplay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <utility>

namespace Oot3dNativeGame {
namespace {

constexpr std::uint32_t kMagic = 0x54414E50U; // "TANP"
constexpr std::uint16_t kVersion = 1U;
constexpr double kSendInterval = 1.0 / 30.0;
constexpr double kTimeoutSeconds = 3.0;

struct WirePose {
  std::uint32_t Magic = kMagic;
  std::uint16_t Version = kVersion;
  std::uint8_t Type = 0;
  std::uint8_t Player = 0;
  std::uint32_t Sequence = 0;
  float X = 0.0F;
  float Y = 0.0F;
  float Z = 0.0F;
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
  std::uint16_t Reserved = 0;
  std::int32_t EntranceId = 0;
  std::uint64_t SwitchFlags = 0;
  std::uint64_t CollectibleFlags = 0;
  std::uint32_t SceneSerial = 0;
};

static_assert(sizeof(WirePose) == 88U);

void SetError(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
}

} // namespace

NetplaySession::~NetplaySession() { Stop(); }

bool NetplaySession::Start(const NetplayOptions& options, std::string* error) {
  Stop();
  mOptions = options;
  if (!mOptions.Enabled) return true;
  mOptions.Port = mOptions.Port == 0U ? 27888U : mOptions.Port;
  mOptions.Player = mOptions.Player == 2U ? 2U : 1U;

  mSocket = socket(AF_INET, SOCK_DGRAM, 0);
  if (mSocket < 0) {
    SetError(error, std::string("netplay socket: ") + std::strerror(errno));
    return false;
  }
  const int flags = fcntl(mSocket, F_GETFL, 0);
  if (flags < 0 || fcntl(mSocket, F_SETFL, flags | O_NONBLOCK) < 0) {
    SetError(error, std::string("netplay nonblocking socket: ") +
                        std::strerror(errno));
    Stop();
    return false;
  }
  const int reuse = 1;
  setsockopt(mSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  if (mOptions.Host) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(mOptions.Port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(mSocket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
      SetError(error, std::string("netplay bind: ") + std::strerror(errno));
      Stop();
      return false;
    }
  } else {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(mOptions.Port);
    if (inet_pton(AF_INET, mOptions.Address.c_str(), &peer.sin_addr) != 1) {
      SetError(error, "netplay address is not a valid IPv4 address");
      Stop();
      return false;
    }
    mPeerAddress = {static_cast<std::uint8_t>((ntohl(peer.sin_addr.s_addr) >> 24U) & 0xFFU),
                    static_cast<std::uint8_t>((ntohl(peer.sin_addr.s_addr) >> 16U) & 0xFFU),
                    static_cast<std::uint8_t>((ntohl(peer.sin_addr.s_addr) >> 8U) & 0xFFU),
                    static_cast<std::uint8_t>(ntohl(peer.sin_addr.s_addr) & 0xFFU)};
    mPeerPort = mOptions.Port;
    mPeerKnown = true;
    // Keep the connected address in the same form as host-discovered peers.
    mPeerAddress = {static_cast<std::uint8_t>(mPeerAddress[0]),
                    static_cast<std::uint8_t>(mPeerAddress[1]),
                    static_cast<std::uint8_t>(mPeerAddress[2]),
                    static_cast<std::uint8_t>(mPeerAddress[3])};
  }

  mEnabled = true;
  mElapsedSeconds = 0.0;
  mSendAccumulator = kSendInterval;
  mLastReceiveSeconds = 0.0;
  mSequence = 0;
  mHasRemote = false;
  mRemote = {};
  return true;
}

void NetplaySession::Stop() {
  if (mSocket >= 0) {
    if (mEnabled && mPeerKnown) {
      NetplayPose empty;
      SendPacket(PacketType::Bye, empty);
    }
    close(mSocket);
    mSocket = -1;
  }
  mEnabled = false;
  mPeerKnown = false;
  mAcknowledged = false;
  mPeerPort = 0;
  mPeerAddress = {};
  mHasRemote = false;
}

bool NetplaySession::Connected() const noexcept {
  if (!mEnabled) return false;
  if (mOptions.Host) {
    return mPeerKnown && mLastReceiveSeconds + kTimeoutSeconds >= mElapsedSeconds;
  }
  return mAcknowledged && mLastReceiveSeconds + kTimeoutSeconds >= mElapsedSeconds;
}

bool NetplaySession::LatestRemotePose(NetplayPose* pose) const noexcept {
  if (pose == nullptr || !mHasRemote) return false;
  *pose = mRemote;
  return true;
}

void NetplaySession::SendPacket(PacketType type, const NetplayPose& pose) {
  if (mSocket < 0 || !mPeerKnown) return;
  WirePose packet;
  packet.Type = static_cast<std::uint8_t>(type);
  packet.Player = mOptions.Player;
  packet.Sequence = mSequence;
  packet.X = pose.Position[0];
  packet.Y = pose.Position[1];
  packet.Z = pose.Position[2];
  packet.Yaw = pose.Yaw;
  packet.Animation = pose.Animation;
  packet.AnimationFrame = pose.AnimationFrame;
  packet.AnimationSpeed = pose.AnimationSpeed;
  packet.AnimationStart = pose.AnimationStart;
  packet.AnimationEnd = pose.AnimationEnd;
  packet.SceneId = pose.SceneId;
  packet.RoomId = pose.RoomId;
  packet.DayTime = pose.DayTime;
  packet.Health = pose.Health;
  packet.Wallet = pose.Wallet;
  packet.EntranceId = pose.EntranceId;
  packet.SwitchFlags = pose.SwitchFlags;
  packet.CollectibleFlags = pose.CollectibleFlags;
  packet.SceneSerial = pose.SceneSerial;

  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(mPeerPort);
  peer.sin_addr.s_addr = htonl((static_cast<std::uint32_t>(mPeerAddress[0]) << 24U) |
                               (static_cast<std::uint32_t>(mPeerAddress[1]) << 16U) |
                               (static_cast<std::uint32_t>(mPeerAddress[2]) << 8U) |
                               static_cast<std::uint32_t>(mPeerAddress[3]));
  sendto(mSocket, &packet, sizeof(packet), 0,
         reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}

void NetplaySession::ConsumePacket(const std::uint8_t* bytes, std::size_t size) {
  if (bytes == nullptr || size < sizeof(WirePose)) return;
  WirePose packet{};
  std::memcpy(&packet, bytes, sizeof(packet));
  if (packet.Magic != kMagic || packet.Version != kVersion ||
      packet.Player == 0U || packet.Player > 2U ||
      packet.Player == mOptions.Player) {
    return;
  }
  const auto type = static_cast<PacketType>(packet.Type);
  if (type == PacketType::Hello) {
    // Poll records the sender address for the host before calling this.
    mAcknowledged = true;
  } else if (type == PacketType::HelloAck) {
    if (!mOptions.Host) mAcknowledged = true;
  } else if (type == PacketType::State) {
    NetplayPose pose;
    pose.Sequence = packet.Sequence;
    pose.Position = {packet.X, packet.Y, packet.Z};
    pose.Yaw = packet.Yaw;
    pose.Animation = packet.Animation;
    pose.AnimationFrame = packet.AnimationFrame;
    pose.AnimationSpeed = packet.AnimationSpeed;
    pose.AnimationStart = packet.AnimationStart;
    pose.AnimationEnd = packet.AnimationEnd;
    pose.SceneId = packet.SceneId;
    pose.RoomId = packet.RoomId;
    pose.DayTime = packet.DayTime;
    pose.Health = packet.Health;
    pose.Wallet = packet.Wallet;
    pose.EntranceId = packet.EntranceId;
    pose.SwitchFlags = packet.SwitchFlags;
    pose.CollectibleFlags = packet.CollectibleFlags;
    pose.SceneSerial = packet.SceneSerial;
    if (!mHasRemote || pose.Sequence >= mRemote.Sequence) {
      mRemote = pose;
      mHasRemote = true;
    }
    mLastReceiveSeconds = mElapsedSeconds;
    if (!mOptions.Host) mAcknowledged = true;
  }
}

void NetplaySession::Poll(double seconds, const NetplayPose& local) {
  if (!mEnabled || mSocket < 0) return;
  seconds = std::max(0.0, std::min(seconds, 0.25));
  mElapsedSeconds += seconds;
  mSendAccumulator += seconds;

  std::array<std::uint8_t, 128> buffer{};
  for (;;) {
    sockaddr_in sender{};
    socklen_t senderSize = sizeof(sender);
    const ssize_t received = recvfrom(
        mSocket, buffer.data(), buffer.size(), 0,
        reinterpret_cast<sockaddr*>(&sender), &senderSize);
    if (received < 0) break;
    const std::uint32_t address = ntohl(sender.sin_addr.s_addr);
    std::array<std::uint8_t, 4> senderBytes{
        static_cast<std::uint8_t>((address >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((address >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((address >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(address & 0xFFU)};
    // A host learns its peer from the first Hello. A client accepts state only
    // from the configured host.
    if (mOptions.Host) {
      mPeerAddress = senderBytes;
      mPeerPort = ntohs(sender.sin_port);
      mPeerKnown = true;
    } else if (senderBytes != mPeerAddress || ntohs(sender.sin_port) != mPeerPort) {
      continue;
    }
    ConsumePacket(buffer.data(), static_cast<std::size_t>(received));
    mLastReceiveSeconds = mElapsedSeconds;
    if (mOptions.Host) {
      // Answer a Hello as soon as the peer is known.
      const WirePose* packet = reinterpret_cast<const WirePose*>(buffer.data());
      if (packet->Type == static_cast<std::uint8_t>(PacketType::Hello)) {
        NetplayPose empty;
        SendPacket(PacketType::HelloAck, empty);
      }
    }
  }

  if (!mPeerKnown) {
    // The client knows the configured host; the host waits for Hello.
    if (mOptions.Host) return;
  }
  if (!mOptions.Host && !mAcknowledged) {
    if (mSendAccumulator >= 0.5) {
      mSendAccumulator = 0.0;
      NetplayPose hello;
      SendPacket(PacketType::Hello, hello);
    }
    return;
  }
  if (mSendAccumulator < kSendInterval) return;
  mSendAccumulator = 0.0;
  ++mSequence;
  NetplayPose pose = local;
  pose.Sequence = mSequence;
  SendPacket(PacketType::State, pose);
}


bool CanApplyRemoteWorld(const NetplayPose& local, const NetplayPose& remote) {
  return local.SceneId != 0U && remote.SceneId == local.SceneId;
}

NetplayPose InterpolateNetplayPose(const NetplayPose& from,
                                   const NetplayPose& to, float alpha) {
  NetplayPose pose = to;
  const float t = std::clamp(alpha, 0.0F, 1.0F);
  for (size_t axis = 0; axis < 3U; ++axis)
    pose.Position[axis] =
        from.Position[axis] + (to.Position[axis] - from.Position[axis]) * t;
  const double delta = std::remainder(
      static_cast<double>(to.Yaw - from.Yaw), 6.28318530717958647692);
  pose.Yaw = static_cast<float>(from.Yaw + delta * t);
  return pose;
}

std::int16_t NetplayYawToBinaryAngle(float radians) {
  constexpr float kScale = 32768.0F / 3.14159265358979323846F;
  const float value = std::round(radians * kScale);
  return static_cast<std::int16_t>(
      std::max(-32768.0F, std::min(32767.0F, value)));
}

NetplayScreenPoint ProjectNetplayPoint(
    const NetplayPose& pose, const std::array<float, 3>& eye,
    const std::array<float, 3>& at, float left, float right, float bottom,
    float top, float nearPlane, std::uint32_t width, std::uint32_t height) {
  NetplayScreenPoint result;
  if (width == 0U || height == 0U || !(nearPlane > 0.0F) ||
      !(right > left) || !(top > bottom)) {
    return result;
  }
  const auto subtract = [](const std::array<float, 3>& a,
                           const std::array<float, 3>& b) {
    return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  const auto dot = [](const std::array<float, 3>& a,
                      const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  const auto cross = [](const std::array<float, 3>& a,
                        const std::array<float, 3>& b) {
    return std::array<float, 3>{a[1] * b[2] - a[2] * b[1],
                                a[2] * b[0] - a[0] * b[2],
                                a[0] * b[1] - a[1] * b[0]};
  };
  auto forward = subtract(at, eye);
  const float forwardLength = std::sqrt(dot(forward, forward));
  if (!(forwardLength > 1.0e-5F)) return result;
  for (auto& component : forward) component /= forwardLength;
  const std::array<float, 3> worldUp{0.0F, 1.0F, 0.0F};
  auto rightAxis = cross(forward, worldUp);
  const float rightLength = std::sqrt(dot(rightAxis, rightAxis));
  if (!(rightLength > 1.0e-5F)) return result;
  for (auto& component : rightAxis) component /= rightLength;
  const auto upAxis = cross(rightAxis, forward);
  const auto relative = subtract(pose.Position, eye);
  const float z = dot(relative, forward);
  if (!(z > nearPlane)) return result;
  const float tangentX = (right - left) / (2.0F * nearPlane);
  const float tangentY = (top - bottom) / (2.0F * nearPlane);
  if (!(tangentX > 1.0e-5F) || !(tangentY > 1.0e-5F)) return result;
  const float ndcX = dot(relative, rightAxis) / (z * tangentX);
  const float ndcY = dot(relative, upAxis) / (z * tangentY);
  if (!std::isfinite(ndcX) || !std::isfinite(ndcY) ||
      std::abs(ndcX) > 1.5F || std::abs(ndcY) > 1.5F) {
    return result;
  }
  result.Visible = true;
  result.X = (ndcX * 0.5F + 0.5F) * static_cast<float>(width);
  result.Y = (1.0F - (ndcY * 0.5F + 0.5F)) * static_cast<float>(height);
  return result;
}

} // namespace Oot3dNativeGame
