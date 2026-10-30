#include "oot3d_netplay.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "oot3d_netplay_tests: " << message << '\n';
    std::exit(1);
  }
}

void Pump(Oot3dNativeGame::NetplaySession& host,
          Oot3dNativeGame::NetplaySession& client, int iterations) {
  Oot3dNativeGame::NetplayPose hostPose;
  hostPose.Position = {1.0F, 2.0F, 3.0F};
  hostPose.Yaw = 0.5F;
  hostPose.Animation = 11U;
  hostPose.AnimationFrame = 12.5F;
  hostPose.AnimationSpeed = 1.25F;
  hostPose.AnimationStart = 2.0F;
  hostPose.AnimationEnd = 30.0F;
  hostPose.SceneId = 0x55U;
  hostPose.RoomId = 2U;
  hostPose.DayTime = 0x8123U;
  hostPose.Health = 0x30U;
  hostPose.Wallet = 42U;
  hostPose.EntranceId = 0x11;
  hostPose.SwitchFlags = 0x0102030405060708ULL;
  hostPose.CollectibleFlags = 0x0807060504030201ULL;
  hostPose.SceneSerial = 9U;
  Oot3dNativeGame::NetplayPose clientPose;
  clientPose.Position = {-4.0F, 5.0F, 6.0F};
  clientPose.Yaw = -0.25F;
  clientPose.Animation = 22U;
  clientPose.AnimationFrame = 3.5F;
  clientPose.AnimationSpeed = 0.75F;
  clientPose.AnimationStart = 1.0F;
  clientPose.AnimationEnd = 20.0F;
  clientPose.SceneId = 0x3EU;
  clientPose.RoomId = 7U;
  clientPose.DayTime = 0x4000U;
  clientPose.Health = 0x2AU;
  clientPose.Wallet = 99U;
  clientPose.EntranceId = 0x2A;
  clientPose.SwitchFlags = 0x1122334455667788ULL;
  clientPose.CollectibleFlags = 0x8877665544332211ULL;
  clientPose.SceneSerial = 7U;
  for (int i = 0; i < iterations; ++i) {
    host.Poll(1.0 / 60.0, hostPose);
    client.Poll(1.0 / 60.0, clientPose);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

} // namespace

int main() {
  const std::uint16_t port = static_cast<std::uint16_t>(
      28000U + (static_cast<unsigned>(getpid()) % 1000U));
  Oot3dNativeGame::NetplayOptions hostOptions;
  hostOptions.Enabled = true;
  hostOptions.Host = true;
  hostOptions.Port = port;
  hostOptions.Player = 1;
  Oot3dNativeGame::NetplayOptions clientOptions;
  clientOptions.Enabled = true;
  clientOptions.Host = false;
  clientOptions.Address = "127.0.0.1";
  clientOptions.Port = port;
  clientOptions.Player = 2;

  Oot3dNativeGame::NetplaySession host;
  Oot3dNativeGame::NetplaySession client;
  std::string error;
  Require(host.Start(hostOptions, &error), error.c_str());
  Require(client.Start(clientOptions, &error), error.c_str());
  Pump(host, client, 120);
  Require(host.Connected() && client.Connected(),
          "loopback sessions did not establish a connection");
  Oot3dNativeGame::NetplayPose hostSees;
  Oot3dNativeGame::NetplayPose clientSees;
  Require(host.LatestRemotePose(&hostSees), "host has no remote pose");
  Require(client.LatestRemotePose(&clientSees), "client has no remote pose");
  Require(hostSees.Position[0] == -4.0F && hostSees.Position[1] == 5.0F &&
              hostSees.Position[2] == 6.0F && hostSees.Yaw == -0.25F &&
              hostSees.Animation == 22U && hostSees.AnimationFrame == 3.5F &&
              hostSees.AnimationSpeed == 0.75F &&
              hostSees.AnimationStart == 1.0F &&
              hostSees.AnimationEnd == 20.0F &&
              hostSees.SceneId == 0x3EU &&
              hostSees.RoomId == 7U && hostSees.DayTime == 0x4000U &&
              hostSees.Health == 0x2AU && hostSees.Wallet == 99U &&
              hostSees.EntranceId == 0x2A &&
              hostSees.SwitchFlags == 0x1122334455667788ULL &&
              hostSees.CollectibleFlags == 0x8877665544332211ULL &&
              hostSees.SceneSerial == 7U,
          "host received the wrong client pose");
  Require(clientSees.Position[0] == 1.0F && clientSees.Position[1] == 2.0F &&
              clientSees.Position[2] == 3.0F && clientSees.Yaw == 0.5F &&
              clientSees.Animation == 11U &&
              clientSees.AnimationFrame == 12.5F &&
              clientSees.AnimationSpeed == 1.25F &&
              clientSees.AnimationStart == 2.0F &&
              clientSees.AnimationEnd == 30.0F &&
              clientSees.SceneId == 0x55U &&
              clientSees.RoomId == 2U && clientSees.DayTime == 0x8123U &&
              clientSees.Health == 0x30U && clientSees.Wallet == 42U &&
              clientSees.EntranceId == 0x11 &&
              clientSees.SwitchFlags == 0x0102030405060708ULL &&
              clientSees.CollectibleFlags == 0x0807060504030201ULL &&
              clientSees.SceneSerial == 9U,
          "client received the wrong host pose");
  Require(hostSees.Sequence != 0U && clientSees.Sequence != 0U,
          "pose sequence was not transported");
  Oot3dNativeGame::NetplayPose localWorld;
  localWorld.SceneId = 0x55U;
  Oot3dNativeGame::NetplayPose remoteWorld;
  remoteWorld.SceneId = 0x55U;
  Require(Oot3dNativeGame::CanApplyRemoteWorld(localWorld, remoteWorld),
          "same-scene remote world was rejected");
  remoteWorld.SceneId = 0x56U;
  Require(!Oot3dNativeGame::CanApplyRemoteWorld(localWorld, remoteWorld),
          "cross-scene remote world was accepted");
  localWorld.SceneId = 0U;
  remoteWorld.SceneId = 0U;
  Require(!Oot3dNativeGame::CanApplyRemoteWorld(localWorld, remoteWorld),
          "pre-play remote world was accepted");
  localWorld.SceneId = 0x55U;
  remoteWorld.SceneId = 0x55U;
  localWorld.SceneSerial = 3U;
  remoteWorld.SceneSerial = 3U;
  Require(Oot3dNativeGame::CanApplyRemoteResources(localWorld, remoteWorld),
          "post-transition resources were rejected");
  localWorld.SceneSerial = 0U;
  Require(!Oot3dNativeGame::CanApplyRemoteResources(localWorld, remoteWorld),
          "pre-transition resources were accepted");
  Require(Oot3dNativeGame::NetplayYawToBinaryAngle(0.0F) == 0,
          "zero yaw did not convert");
  Require(Oot3dNativeGame::NetplayYawToBinaryAngle(3.14159265358979323846F) ==
              32767,
          "pi yaw did not clamp");
  Require(Oot3dNativeGame::NetplayYawToBinaryAngle(
              -3.14159265358979323846F) == -32768,
          "negative pi yaw did not clamp");
  Oot3dNativeGame::NetplayPose interpolateFrom;
  interpolateFrom.Position = {0.0F, 0.0F, 0.0F};
  interpolateFrom.Yaw = 3.0F;
  Oot3dNativeGame::NetplayPose interpolateTo = interpolateFrom;
  interpolateTo.Position = {10.0F, 4.0F, -2.0F};
  interpolateTo.Yaw = -3.0F;
  interpolateTo.SceneId = 7U;
  const auto halfway = Oot3dNativeGame::InterpolateNetplayPose(
      interpolateFrom, interpolateTo, 0.5F);
  Require(halfway.Position[0] == 5.0F && halfway.Position[1] == 2.0F &&
              halfway.Position[2] == -1.0F && halfway.SceneId == 7U,
          "pose interpolation lost position/state");
  Require(std::abs(std::abs(halfway.Yaw) - 3.14159265358979323846F) < 0.001F,
          "pose interpolation did not take the short yaw path");
  Oot3dNativeGame::NetplayPose projectionPose;
  projectionPose.Position = {0.0F, 0.0F, 0.0F};
  const auto centered = Oot3dNativeGame::ProjectNetplayPoint(
      projectionPose, {0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, 0.0F},
      -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, 100U, 50U);
  Require(centered.Visible && std::abs(centered.X - 50.0F) < 0.01F &&
              std::abs(centered.Y - 25.0F) < 0.01F,
          "center projection is wrong");
  projectionPose.Position = {20.0F, 0.0F, 0.0F};
  Require(!Oot3dNativeGame::ProjectNetplayPoint(
              projectionPose, {0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, 0.0F},
              -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, 100U, 50U).Visible,
          "offscreen remote pose was accepted");
  projectionPose.Position = {0.0F, 0.0F, 20.0F};
  Require(!Oot3dNativeGame::ProjectNetplayPoint(
              projectionPose, {0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, 0.0F},
              -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, 100U, 50U).Visible,
          "pose behind the camera was accepted");
  host.Stop();
  client.Stop();
  Require(!host.Connected() && !client.Connected(),
          "stopped sessions still report a connection");
  std::cout << "oot3d_netplay_tests: ok\n";
  return 0;
}
