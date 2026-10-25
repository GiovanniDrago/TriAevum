# Two-Window Netplay Evaluation

**Status**: M0 launcher, M1 loopback pose transport and M2 remote-player proxy are usable. M2.5 has an experimental render-only overlay, disabled by default because cloned Player draws still show a skinned-model artifact. An M3 scaffold synchronizes scene/room identity, clock, health and wallet; full flags, doors, items, enemies and cutscenes remain pending.

## Goal

Two playable Links, each controlled by a different controller, with one shared
world. The target is not a split-screen renderer: each player runs a full
TriAevum instance in its own window, locally or over LAN.

## Recommendation

Use a **host-authoritative state-sync** architecture, matching the Ocarina of
Time co-op work in `coop-anything`:

- each process runs the full game and has one local Link;
- the remote Link is a puppet/avatar driven by snapshots, not a second
  full Player actor;
- the host owns saves, scene flags, items, enemy authority and cutscenes;
- the client owns its own Link pose, animation and camera;
- loopback UDP is the first transport; LAN and relay are later.

This is the same shape as Ship of Harkinian + Anchor. OOT True Co-op is the
closest reference because it explicitly runs two windows on one PC with two
controllers and host-authority enemy sync.

## What Nucleus Co-op / SplitScreenMe Arch confirms

The local `splitscreenme-arch` checkout has two relevant layers: the Windows
Nucleus runtime and a Linux/Proton reimplementation.

**Remnant: From the Ashes is the better reference.** It has a real JS handler
with per-instance save/config paths, game-folder symlinks, XInputPlus controller
remapping, ProtoInput focus handling, and Goldberg/Nemirtingas online identity
emulation. The working pattern is:

- one process per player;
- one XInput controller index per process (`Controller1 = GamepadId + 1`);
- per-player `USERPROFILE`/`APPDATA`/`LOCALAPPDATA` and per-player save/config;
- the game's own public-game/quickplay path for connection;
- focus spoofing so both processes keep reading input while another window has
  OS focus.

**Remnant 2 is the fallback, not the model.** In this checkout it has no JS
handler, only native C# preparation. It is Steam-only, fails open when EOS or
Goldberg assets are missing, depends on the default app settings to apply its
focus/resolution fix, has incomplete process cleanup, and its generic handler
metadata is actually Remnant 1 metadata. It is useful for save-discovery and
per-player-prefix ideas, not as the main reference.

The Linux/Proton port contributes directly reusable ideas for a native game:

- evdev grab plus a virtual Xbox 360 `uinput` pad per player;
- a per-instance controller allow-list and optional bubblewrap device isolation;
- per-player Wine prefixes and workspaces;
- a placement watchdog and per-session diagnostics.

It also exposes gaps that matter for TriAevum: no keyboard routing, no
Windows-style focus spoofing, no final process supervision on stop, and an
incomplete Remnant 1 `Game.Play` implementation.

For TriAevum, do not port XInputPlus, ProtoInput, Goldberg, Nemirtingas, DLL
injection or Wine prefixes. Keep the architecture: **supervisor + one process,
identity, controller and data root per player**, and implement the transport
natively.

## Launcher decision

A **native launcher/supervisor** is the right place for mode and controller
choice. It should not be a dialog inside the already-running guest:

1. Start `TriAevum Launcher` before any game process.
2. Choose `Single player` or `Multiplayer / two players`.
3. In multiplayer, show only connected gamepads and require two different pads
   (keyboard and mouse excluded for now).
4. Choose save profiles: fresh, import a copy of the user's save, or keep the
   per-player saves already assigned.
5. Create two private instance roots:
   - save directory;
   - game config and controls config;
   - renderer/shader cache and logs;
   - runtime output.
6. Launch the host and client with explicit flags:
   - `--player 1/2`;
   - `--controller-path` or ordinal;
   - `--save-data`, `--config`, `--controls-config`;
   - `--netplay-host` or `--netplay-connect=127.0.0.1:<port>`.
7. Keep a parent process that owns child PIDs, window placement, the input
   router and cleanup.

Single-player stays the current one-instance path. Multiplayer never shares a
save directory, and the client must not write the host save.

## Controller audit

Two wired Zikway HID gamepads are connected. They are identical in name, VID/PID,
descriptor and SDL GUID, and neither exposes a serial number. Their stable
distinction is the physical USB port path (two different ports), not the SDL
index, `jsN`, `eventN`, GUID or serial.

The current TriAevum controls config has no controller pin (`controller_guid`
empty, no `controller_serial`). `ResolveSdlController` matches GUID/serial and
falls back to the lowest SDL instance. Two instances would therefore both bind
the same first pad. The in-process dog split uses device order and is not a
cross-process solution.

Required before any two-instance controller test:

- add a per-instance `controller_path` (preferred, using the game controller's
  device path / stable USB path) or an equivalent ordinal selector;
- exclude Steam virtual gamepads from the player mapping unless explicitly
  requested;
- pass a separate controls config to each instance;
- verify that each process receives only its own pad while the other window is
  focused;
- add a graceful "controller disconnected" prompt, because wireless identity
  may not be stable and identical pads have no serial.


References:

- `coop-anything/playbook/networking.md`: host-authoritative state sync,
  snapshot interpolation, stable IDs, host save.
- `coop-anything/playbook/split-screen.md`: split-screen is a layer over
  multiplayer; a single-player game needs co-op first.
- `coop-anything/playbook/multi-instance-testing.md`: private saves/settings,
  controller isolation, per-process logs and resource limits.
- `coop-anything/games/ocarina-of-time/README.md`: Anchor and OOT True Co-op.
- `https://github.com/bghill95/OOT-True-Co-op`: two windows, two controllers,
  shared HP, host-authority enemy mirroring, controller pinning.

## Why not the current split renderer

The current split-screen experiment reduces the scene to one presented camera
and has corrupted lighting/frozen presentation. The two-Link target does not
need a second viewport in one guest. Each process renders one full camera, so
the split renderer is not on this path.

## Architecture

```text
Process A (host)                         Process B (client)
  local Link A                             local Link B
  authoritative world                      mirrored world
  authoritative save                       save disabled
  remote Link B puppet                     remote Link A puppet
  UDP socket <──────── loopback/LAN ──────> UDP socket
```

### Ownership

| State | Owner |
| --- | --- |
| Local Link position/yaw/animation | local client |
| Remote Link presentation | receiving client, interpolated |
| Camera | local client |
| World flags, doors, chests, collectibles | host |
| Inventory, equipment, health, wallet | host, mirrored |
| Enemies and bosses | host authority; client sends validated hits |
| Cutscenes/dialogue | host drives, client watches/gates |
| Save/load | host; joining client receives a snapshot |

### Remote Link

The remote Link must not run the full `Player` logic in the receiving process.
Two Player actors would compete for global "the player" references, camera,
damage and story systems. Build a dedicated puppet path:

- spawn/reuse an actor or render-only model for the remote Link;
- copy position, yaw, body scale, animation id/frame, equipment visuals;
- disable local input, AI, damage ownership and story callbacks on the puppet;
- hide the puppet while the local player is in a scene/room it does not share.

### Network messages

- handshake: protocol/build hash, ROM/scene identity, player slot, controller;
- unreliable snapshots: local Link pose/animation at 20-30 Hz;
- reliable events: scene/room change, doors, flags, items, damage, save;
- periodic state hash for desync detection;
- stable IDs only: scene id, flag index, actor identity, never guest pointers.

### Transport

- First: ENet (MIT) over loopback UDP.
- Same code works on LAN with a host IP.
- Steam relay or GameNetworkingSockets is a later option; do not start there.
- Reliable-ordered is for events only; movement is unreliable and interpolated.

## Milestones

### M0 - Two isolated windows

- launch two clean instances;
- separate save-data, config, logs and caches;
- add controller-path/ordinal selection so each identical pad is assigned to a
  different process;
- controller A only in window A, controller B only in window B;
- both continue while unfocused;
- measure CPU/GPU/RAM.

No networking and no gameplay changes.

### M1 - Session

- ENet host/client handshake over loopback;
- player slots and scene identity;
- heartbeat, disconnect, rejoin;
- no world sync yet.

### M2 - Remote Link

- export local Link pose/animation;
- remote puppet in the other process;
- interpolation and visibility;
- two controllers drive two different Links.

This is the first useful co-op milestone.

### M3 - Shared world

- scene/room transition;
- scene flags, doors, chests, collectibles;
- inventory/equipment/health/wallet;
- time of day;
- host save and join snapshot.

### M4 - Enemies and story

- host-authority enemy AI and boss adapters;
- validated client hits;
- cutscene/dialogue gating;
- room clear and save/reload;
- LAN and network chaos testing.

## Testing

- Two clients on one PC, two controllers, separate save copies.
- Scripted controller input per instance; probe positions, animation, flags.
- State hash exchanged periodically; first divergence is logged.
- Kill client/host, reconnect, scene change, save/load, cutscene.
- Add lag/loss/reorder with `chaos` or Linux `tc netem`.
- Evidence must include both windows, both Links, one shared world state, and
  exact log/probe paths.

## Licensing and repository rules

- Use the architecture and public documentation; do not copy unlicensed code.
- No ROM, save, asset, or personal path enters this repository.
- Credit Anchor, OOT True Co-op and Ship of Harkinian if their design is used.
- Keep the current split-screen work on its own branch; this branch starts from
  `origin/main` without the co-op dog changes.

## coop-anything setup

The knowledge repository is independent of TriAevum and needs only Python 3.9+
and PyYAML for its read-only MCP server:

```sh
git clone https://github.com/cranebeaker/coop-anything.git <coop-anything>
python3 -m pip install --user pyyaml
python3 -m unittest <coop-anything>/tools/mcp/test_coop_mcp.py
opencode mcp add coop-anything --global -- \
  python3 <coop-anything>/tools/mcp/coop_mcp.py
opencode mcp list
```

The MCP server is optional; the playbooks are Markdown files.

Verified in this evaluation:

- clone size about 9 MB; no build or game install required;
- Python 3.14.8 with PyYAML 6.0.3 present;
- `python3 -m unittest tools/mcp/test_coop_mcp.py` ran 13 tests, all OK;
- OpenCode registered `coop-anything` globally and reports it **connected**;
- `list_docs` returns the playbook, game, skill and test-tool inventory;
- `get_skill` for `coop-triage`, `coop-networking` and `coop-decomp-port`
  returned the expected guidance;
- `search_directory` for Ocarina of Time returned Anchor, OOT True Co-op,
  Shipwright-Online and ocarina-coop.

No additional MCP (REA or a reverse-engineering server) is required for the
two-window/network evaluation.


## Implemented M0-M2

### M0 - launcher and isolation

`tools/netplay/triaevum_launcher.cpp` builds as `triaevum_launcher`. It:

- lists physical SDL gamepads and hides Steam virtual mirrors by default;
- requires two distinct pads for multiplayer;
- writes a per-instance `oot3d_controls.json` with `controller_path`, GUID
  and serial, and disables keyboard/mouse for the multiplayer mode;
- copies the selected save seed into two private save roots;
- writes per-instance game/controls configs and output/log paths;
- gives each instance its own renderer cache, optionally seeded through
  `--cache-source`;
- waits for the first instance's `TopScreen source-port layout` marker before
  starting the second, instead of launching both cold;
- owns child PIDs and stops only its own processes;
- supports `--dry-run` planning, `--no-save-copy`, `--duration`, `--netplay-port`
  and `--netplay-connect-address`.

A two-pad path audit is included: the two connected Zikway pads have identical
GUIDs and no serial, so `controller_path` is mandatory for correct pinning.

### M1 - loopback/LAN pose transport

`oot3d_netplay.h/.cpp` adds a minimal UDP host/client session:

- `--netplay-host <port>` / `--netplay-connect host:port`;
- `--netplay-player 1|2`;
- Hello/HelloAck/State/Bye packets, 30 Hz pose snapshots, heartbeat timeout;
- local Link position/yaw read from the validated Player actor;
- optional `--netplay-probe` JSONL with connection and remote pose.

The launcher wires p1 as host and p2 as client on `127.0.0.1:27888` by default.
Two real instances connected in the spike and both wrote netplay probes with
`connected=true` and remote poses.

### M2 - remote-player proxy

The local process projects the synchronized remote position through the scene
camera and draws a labeled P2/P1 marker over the presented frame. The projection
is a pure function with tests. This is a **proxy**, not the final second Link
model: spawning/animating a second full Link without becoming a second `Player`
is still the next engine-level task.

### M2.5 - render-only remote Link

The first renderer overlay path is implemented:

- `PicaDrawView::HostReplay` lets a draw be rasterized without consuming the
  native composition schedule or repeating native bookkeeping;
- the scheduler accepts a `NetplayOverlayDraw` with local/remote pose and actor
  address;
- scene draws whose native actor identity matches the local Player are cloned;
- the clone's model rows are transformed by the remote translation and yaw;
- the overlay uses the local mesh/animation and skips native temporal/shadow/
  grass side effects.

The overlay is **experimental and off by default**. Enabling it with
`TRIAEVUM_NETPLAY_REMOTE_LINK=1` clones the Player actor's scene draws and
currently produces a stretched green mesh from the skinned body draws. A
baseline capture of the same bed scene has no artifact; the clone is therefore
not accepted as M2.5. The default path remains the M2 projected P1/P2 marker.
A correct second Link needs a guest-side remote actor (or a verified skinning
palette transform), not a host replay of the local skinned draw list.

### M3 - world-state scaffold

The transport now carries `SceneId`, `RoomId`, `DayTime`, `Health` and
`Wallet`. The host reads them from the native `PlayState`/`SaveContext`; the
client applies clock, health and wallet only when both sessions report the same
non-zero scene (`CanApplyRemoteWorld`). The client keeps its own save and never
writes the host save. This is not full world replication: scene transitions,
flags, doors, chests, items, enemies and cutscenes still diverge.

### Verification

- `oot3d_netplay_tests`: protocol loopback, pose/world round-trip,
  same-scene application policy, center/offscreen/behind-camera projection.
- LLVM coverage for `oot3d_netplay.cpp`: 90.53% lines, 92.86% functions,
  88.66% regions.
- `oot3d_native_a32_input_tests`: controller path/GUID/serial round-trip and
  legacy config compatibility.
- `three_ds_sdl_controller_tests`: SDL selection and device behavior.
- Two real instances connected over loopback and exchanged poses; cold-start
  shader preparation remains the main launcher timing issue, handled by the
  readiness wait and warm-cache source.
- Visual check: the experimental overlay produced
  `TRIAEVUM_NETPLAY_OVERLAY actor=...` and cloned draws, but the framebuffer
  showed a stretched skinned-mesh artifact. A no-netplay baseline capture of
  the same scene was clean. M2.5 visual PASS is therefore not claimed.
- `title_to_existing_save.json` now selects the occupied save slot with
  `circle_y` before the A press; without it the run stops at name entry.

## First spike result

The clean `netplay-eval` branch built `oot3d_native_game` successfully with
`THREE_DS_RECOMP_ENABLE_SSSR=OFF` and the user's SPDLOG prefix.

Two clean instances were launched on one PC with separate save-data, controls
config, output and runtime cache directories. Both processes started; the
second was not rejected by a single-instance lock, and both wrote their own
`runtime.json` and save copy.

The spike exposed a startup cost that must be handled before controller or
network tests:

- both instances compiled shaders at the same time;
- the first instance reached 757 frames in a 45 s window; the second remained at
  0 frames and was killed by the same window;
- memory peaked around 4 to 5 GB per instance during shader preparation;
- a copied user shader cache did not fully eliminate the second instance's
  preparation.

Required next step for the two-window harness: warm one instance to a known
ready state, copy its completed renderer cache to the second instance, then
start the second instance sequentially with a longer readiness timeout. Do not
launch both cold clients simultaneously.

The controller audit found two wired, identical Zikway pads with no serial and
the same SDL GUID. They are distinguishable by USB port path, but TriAevum has
no controller-path/ordinal setting yet, so both instances would select the same
pad. Controller isolation is therefore **not solved** until the launcher and
input selection are extended. The in-process dog split already consumes both
pads in one instance; that is not usable for two independent processes.

## Open risks

- OoT3D's global Player assumptions; the puppet must not be a second Player.
- Scene flags and actor-internal state are harder than player pose.
- Cutscenes, room transitions and saves need explicit gating.
- Two full instances cost roughly twice the CPU/GPU/RAM.
- Two identical controllers may need per-instance ordinal selection if serial
  numbers are unavailable.
- Host authority is required for story; client-authoritative enemies would
  desync.
