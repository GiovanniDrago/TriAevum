# Two-Window Netplay Evaluation

**Status**: evaluation and spike plan; no netplay implementation yet.

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

No physical controllers were connected during this spike, so controller
isolation was not executed. The configuration exposes `controller_guid` and
`controller_serial`; two different pads need a hardware test, and identical
pads may require an ordinal/index selector.

## Open risks

- OoT3D's global Player assumptions; the puppet must not be a second Player.
- Scene flags and actor-internal state are harder than player pose.
- Cutscenes, room transitions and saves need explicit gating.
- Two full instances cost roughly twice the CPU/GPU/RAM.
- Two identical controllers may need per-instance ordinal selection if serial
  numbers are unavailable.
- Host authority is required for story; client-authoritative enemies would
  desync.
