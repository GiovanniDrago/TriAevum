# Feature Specification: Co-op Dog Rupee Pickup

**Feature Branch**: `001-fix-dog-rupee-pickup` (branch not created — no `before_specify` hook is configured)

**Created**: 2026-10-23

**Status**: Draft

**Input**: User description: "Il P2 che utilizza il cane passa attraverso alle rupie senza raccoglierle, va fatta una fix e deve raccogliere le rupie come quando le prende Link"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - P2 Dog Collects Ground Rupees (Priority: P1)

A second player controlling the dog companion walks the dog over a rupee lying on the ground. The rupee is collected, disappears from the world, and the shared wallet increases by that rupee's value, exactly as when Link walks over the same rupee.

**Why this priority**: This is the reported defect. Without it, P2 cannot participate in the core economy loop and co-op play feels broken. Everything else in this feature is refinement of this behavior.

**Independent Test**: Start a session with two players, place or find a single ground rupee, walk the dog over it, and confirm the rupee disappears, the wallet increases by the correct amount, and the pickup feedback plays. No other story is needed to demonstrate value.

**Acceptance Scenarios**:

1. **Given** an active P2 dog and a collectible green rupee on the ground, **When** the dog walks over the rupee, **Then** the rupee is removed and the shared wallet increases by 1.
2. **Given** an active P2 dog and a collectible blue rupee, **When** the dog walks over it, **Then** the rupee is removed and the wallet increases by 5.
3. **Given** an active P2 dog and a collectible red rupee, **When** the dog walks over it, **Then** the rupee is removed and the wallet increases by 20.
4. **Given** an active P2 dog and a collectible gold rupee, **When** the dog walks over it, **Then** the rupee is removed and the wallet increases by 50.
5. **Given** an active P2 dog and a collectible purple rupee, **When** the dog walks over it, **Then** the rupee is removed and the wallet increases by 200.
6. **Given** the dog approaching a rupee at normal walking speed, **When** the dog's body reaches the rupee, **Then** collection happens on contact without the dog passing through or requiring a button press.

---

### User Story 2 - Pickup Respects Wallet Capacity (Priority: P2)

When the shared wallet cannot hold the rupee's value, the dog must not credit currency beyond capacity and must not destroy the rupee. The rupee remains available until there is room, and the player receives the normal full-wallet indication instead of silently losing the item.

**Why this priority**: Prevents item loss. It is lower priority only because it requires a wallet that is at or near capacity.

**Independent Test**: Fill the wallet, place a rupee in front of the dog, walk over it, and confirm the rupee remains and the wallet does not change.

**Acceptance Scenarios**:

1. **Given** a full wallet and a ground rupee, **When** the dog walks over the rupee, **Then** the wallet value is unchanged and the rupee stays in the world.
2. **Given** remaining wallet capacity that is greater than or equal to the rupee's value, **When** the dog collects it, **Then** the wallet increases by the full value and the rupee is consumed.
3. **Given** a full wallet, **When** the dog remains standing on a rupee, **Then** the rupee is never silently destroyed and the full-wallet feedback does not repeat every frame.

---

### User Story 3 - Pickup Feedback Matches Link (Priority: P3)

When the dog collects a rupee, the same player-visible feedback that Link triggers is shown: the pickup sound, the sparkle/effect at the pickup position, and the wallet counter update on the HUD.

**Why this priority**: Without feedback, players cannot tell whether collection succeeded, which is exactly the confusion reported. It does not change the economy but it completes the fix.

**Independent Test**: Collect one rupee with Link and one with the dog and compare the visible and audible feedback and the HUD counter update.

**Acceptance Scenarios**:

1. **Given** a dog pickup, **When** the rupee is collected, **Then** the standard rupee pickup sound plays once.
2. **Given** a dog pickup, **When** the rupee is collected, **Then** the standard pickup effect appears at the rupee position.
3. **Given** a dog pickup, **When** the rupee is collected, **Then** the HUD wallet counter reflects the new balance within the same gameplay moment.

---

### Edge Cases

- The dog touches a rupee that has just spawned or is still falling/moving and is not yet collectible: the dog must not collect it early or credit it twice.
- Two rupees or more are inside the dog's pickup area at the same moment: every eligible rupee is collected once, and the wallet increases by the sum.
- Link and the dog reach the same rupee in the same gameplay moment: the value is credited exactly once and the rupee is removed once.
- The dog is teleported, despawned, or reset while standing on rupees: no rupee is collected from the old position and no currency is credited twice.
- The game is paused, a menu is open, a cutscene is playing, or a scene transition is in progress: the dog must not collect rupees during those states.
- The rupee is inside a shop, a chest, a minigame reward, or a scripted sequence: behavior is out of scope and must be unchanged.
- The wallet is one unit below its capacity and the rupee value exceeds the remaining space: the pickup is rejected, the rupee remains, and the player is informed, as with Link.
- The dog is missing, not fully loaded, or in an invalid state: no wallet change and no world change occurs.
- The player releases P2 or the dog is inactive: no dog pickup occurs; Link's pickup is unaffected.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The active P2 dog MUST collect ground rupees by body contact, without requiring a button press, when it reaches the same practical pickup distance used by Link.
- **FR-002**: Collected rupees MUST credit the shared wallet by their correct denomination: green 1, blue 5, red 20, gold 50, purple 200.
- **FR-003**: Each rupee MUST be consumed exactly once and MUST be removed from the world in the same pickup event that credits the wallet.
- **FR-004**: The dog MUST NOT pass through, overlap, or walk past an eligible ground rupee without collecting it during normal movement, including at sprint speed.
- **FR-005**: Rupees MUST NOT be collected before they become collectible by the game's normal rules (for example, while spawning, falling, or locked by a scripted state).
- **FR-006**: When two players or two pickup checks reach the same rupee, the wallet MUST be credited exactly once and the rupee MUST be removed once.
- **FR-007**: When the shared wallet is full, the dog MUST NOT credit currency, MUST leave the rupee in the world, and MUST present the same wallet-full feedback Link receives.
- **FR-008**: A dog pickup MUST produce the same sound, pickup effect, and HUD wallet update as an equivalent Link pickup.
- **FR-009**: Dog rupee collection MUST only occur during active gameplay, never while paused, in menus, in cutscenes, during loading, or during scene transitions.
- **FR-010**: Dog rupee collection MUST verify that the dog and the rupee are valid, present, and initialized before changing wallet or world state; when that check fails it MUST leave both the wallet and the rupee unchanged.
- **FR-011**: If crediting the wallet fails or is rejected, the rupee MUST remain in the world and MUST NOT be destroyed.
- **FR-012**: Dog rupee collection MUST work in every scene that contains ground rupees and MUST NOT depend on per-scene configuration or a specific save state.
- **FR-013**: The feature MUST NOT change Link's rupee pickup behavior, values, feedback, or wallet accounting.

### Key Entities *(include if data involved)*

- **P2 Dog Companion**: The second player's controllable companion. Relevant state: active/joined, valid actor presence, world position, movement state, and control ownership.
- **Ground Rupee**: A collectible currency item in the world. Relevant attributes: denomination/value, position, and whether it is currently collectible.
- **Shared Wallet**: The single currency balance shared by both players. Relevant attributes: current balance and capacity.
- **Pickup Feedback**: The player-visible confirmation of collection: sound, world effect, and wallet counter on the HUD.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a controlled gameplay check, the dog collects 100% of eligible ground rupees it walks over (at least 20 consecutive contacts across all five denominations) with zero pass-throughs.
- **SC-002**: After every dog pickup, the wallet balance delta equals the collected rupee's denomination in 100% of cases.
- **SC-003**: Each collected rupee is removed exactly once; a repeated run of the same pickup path never produces duplicate credit or duplicate removals (0 occurrences).
- **SC-004**: With a full wallet, 100% of dog contacts leave the wallet unchanged and the rupee present in the world (at least 10 contacts).
- **SC-005**: In a shared pickup test where Link and the dog reach one rupee together, the wallet increases exactly once (0 duplicate credits) across 20 attempts.
- **SC-006**: Pickup feedback appears within the same gameplay moment as contact, such that a player perceives no delay (feedback visible within 100 ms of contact).
- **SC-007**: Link's own pickup success rate remains 100%, and existing single-player behavior shows no regression.
- **SC-008**: Gameplay remains at 60 frames per second during a scene with many ground rupees; no measurable slowdown is observed compared with the same scene before the change.
- **SC-009**: All five rupee denominations are supported (5 of 5), with no denomination credited at the wrong value.

## Assumptions

- The two players share one wallet; dog pickups credit the same balance Link uses.
- Scope is limited to ground rupees. Hearts, ammunition, sticks, nuts, bottles, and other collectibles are out of scope for this fix.
- "Like Link" means the same contact-based rules, values, wallet-capacity behavior, and feedback; the dog receives no bonus multiplier or dog-specific pickup radius beyond what is needed to match Link's practical reach.
- The dog is only able to collect while it is active and controllable by P2; an idle or despawned dog does not collect.
- Rupees inside shops, chests, minigames, or scripted rewards continue to follow their existing scripted behavior.
- The reported defect is reproducible with the existing co-op dog runtime; this specification does not introduce a new companion or a new currency system.
- The N64 reference path consumes a paid rupee when the wallet is full. OOT3D parity for that edge case is not yet established, so this specification deliberately chooses the non-destructive behavior (keep the rupee) as a safety-first native prerogative. If OOT3D evidence later shows different behavior, align it in a follow-up rather than silently destroying currency.
- Runtime acceptance requires a legal user-supplied ROM and a scene containing ground rupees, supplied through the documented test harness; compile-only checks are not sufficient evidence.
- No branch was created during this specification step because the project has no `before_specify` hook configured; the current branch is `coop-local`.
