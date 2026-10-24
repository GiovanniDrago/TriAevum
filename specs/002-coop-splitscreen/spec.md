# Feature Specification: Co-op Dynamic Split Screen

**Feature Branch**: `002-coop-splitscreen` (branch not created — no `before_specify` hook is configured)

**Created**: 2026-10-23

**Status**: Draft

**Input**: User description: "Aggiungi la possibilità di splitscreen quando c'è anche il cane in game. Quando i due personaggi sono vicini la telecamera resta una sola, se uno dei due si allontana lo schermo diventa in splitscreen. La UI può restare una sola per il momento, e solo al camera della scene dividersi. Quando il cane non c'è o si preme start e si porta il cane affianco a link, la telecamera non è divisa."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Automatic Split When The Dog Leaves The Shared View (Priority: P1)

Two players are in a normal gameplay session with the dog companion active. While Link and the dog stay together in one camera view, the game shows a single scene view with the normal UI. When one of them moves far enough that the other is no longer framed in that shared view, the scene display splits into two views: one for the player controlling Link and one for the player controlling the dog. When they come back together, the display returns to a single view.

**Why this priority**: This is the requested feature. It lets P2 explore independently without forcing P1's camera to follow the dog, and it restores the familiar single view when the players are together.

**Independent Test**: Start a session with Link and an active P2 dog. Keep them together and confirm one scene view. Move the dog away until it leaves Link's shared view and confirm the display splits. Bring the dog back and confirm the display merges. The same test can be run by moving Link away from the dog.

**Acceptance Scenarios**:

1. **Given** an active dog and both characters remain framed together, **When** gameplay continues, **Then** the scene shows one camera view and one UI.
2. **Given** both characters framed together, **When** one of them leaves the shared view for longer than the short grace period, **Then** the scene splits into two views within the next half second.
3. **Given** the scene is split, **When** both characters are framed together again for the grace period, **Then** the scene merges back to one view within the next half second.
4. **Given** the scene is split, **Then** the Link player sees a view centred on Link and the dog player sees a view centred on the dog.
5. **Given** the scene is split, **Then** the UI remains a single instance on screen and is not duplicated in both views.
6. **Given** the scene is split, **When** the dog moves, **Then** only the dog's view follows the dog; the Link view remains under the Link player's control.

---

### User Story 2 - Single View When The Dog Is Absent Or Summoned Close (Priority: P2)

The dog is not present, or P2 brings the dog back next to Link. In both cases the game must show a single camera view, even if the display was split a moment earlier.

**Why this priority**: Prevents unnecessary split screens and makes the expected behaviour explicit for the most common co-op actions: no dog, P2 Start, and summoning the dog beside Link.

**Independent Test**: With no dog, confirm the display stays single. With the dog active and the display split, press P2 Start / summon so the dog appears next to Link, then confirm the display merges to one view.

**Acceptance Scenarios**:

1. **Given** no dog is present in the current scene, **When** the player moves around, **Then** the scene always uses one camera view.
2. **Given** no dog is present, **When** P2 joins and the dog is created next to Link, **Then** the scene still uses one camera view until the characters separate.
3. **Given** the scene is split and the dog is active, **When** P2 presses Start and the dog is brought next to Link, **Then** the display merges to one view.
4. **Given** the scene is split, **When** the dog disappears, dies, is dismissed, or P2 disconnects, **Then** the display merges to one view.
5. **Given** the dog is present but P2 is not controlling it, **Then** the display stays single.

---

### User Story 3 - Stable And Safe Transitions (Priority: P3)

The single/split transition must not flicker when the characters hover near the boundary, and it must behave safely during menus, cutscenes, scene changes, teleports, and invalid character states.

**Why this priority**: A split screen that flickers or remains active in menus and cutscenes is distracting and can make the game feel broken. It also protects the single-player and no-dog experience.

**Independent Test**: Walk one character back and forth across the split boundary for thirty seconds and confirm the display does not flicker. Enter a cutscene, pause menu, and a new scene while split and confirm the scene returns to a single view until gameplay resumes and the characters actually separate.

**Acceptance Scenarios**:

1. **Given** the characters hover at the boundary, **When** they move back and forth, **Then** the display does not alternate rapidly; there is at most one transition per deliberate crossing plus the grace period.
2. **Given** the scene is split, **When** a cutscene, loading screen, pause menu, or other non-gameplay state begins, **Then** the scene is shown as a single view and the UI remains single.
3. **Given** gameplay resumes after a cutscene or menu, **When** the characters are still separated, **Then** the split re-appears automatically; if they are together, it stays single.
4. **Given** the scene is split, **When** the game changes scene or teleports either character, **Then** the display returns to a single view during the transition and re-evaluates when gameplay resumes.
5. **Given** the dog is in an invalid or uninitialized state, **When** the display would otherwise split, **Then** the scene stays single rather than showing an empty or broken view.
6. **Given** the game runs in split mode in the test scene, **Then** gameplay remains at the target 60 frames per second and both players retain responsive controls.

---

### Edge Cases

- Both characters cross the boundary in the same moment: the display must settle on one stable state, not oscillate.
- One character is within the shared camera frame but hidden behind geometry: the display remains single because the character is still framed; line of sight alone does not trigger a split.
- The two characters are in different rooms or areas: the scene splits only if both views can be prepared; otherwise the system falls back to one view rather than showing a broken or empty panel.
- The dog is summoned while the scene is split but lands far from Link: the display stays split and re-evaluates.
- The dog is summoned while the scene is split and lands beside Link: the display merges.
- The dog dies, despawns, is dismissed, or P2 disconnects while split: the display merges immediately or within the normal grace period.
- Link enters a cutscene, dialogue, or a scripted camera sequence while split: the scene is single for the duration; after it ends, the split state is re-evaluated.
- The game is paused or a menu is open while split: the scene behind the UI is single, and the UI remains a single instance.
- The window is resized or the aspect ratio changes: both views remain usable and the single UI remains legible.
- One view cannot be rendered because the character or camera is not valid: the game falls back to the single view instead of showing a broken panel.
- The dog is present but idle and far away: the scenario is still a split when P2 is active; if P2 is not active, the display is single.
- The characters are close in distance but one is outside the camera frame: the display splits, because "together" means "framed together", not merely near in world distance.
- The characters are far apart but both happen to be visible in a very wide view: the display stays single, because both are still framed together.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST automatically choose between a single scene view and a split scene view whenever an active P2 dog is present.
- **FR-002**: The system MUST use a single scene view while Link and the dog are both framed in the shared view.
- **FR-003**: The system MUST split the scene into two views when either character has been outside the shared view for the defined grace period.
- **FR-004**: The system MUST merge the two views when both characters have been framed together for the defined grace period.
- **FR-005**: In split mode, each player MUST receive a dedicated scene view: the Link view follows Link and the dog view follows the dog.
- **FR-006**: The UI MUST remain a single instance in split mode; the HUD, minimap, buttons, and counters MUST NOT be duplicated per view.
- **FR-007**: Only the scene cameras MUST split; menus, HUD layout, and other non-scene UI MUST remain single.
- **FR-008**: The system MUST use a single view when no dog is present or when P2 is not actively controlling the dog.
- **FR-009**: Bringing the dog next to Link with P2 Start or the summon action MUST merge the display to a single view.
- **FR-010**: If the dog disappears, dies, is dismissed, or P2 disconnects while split, the display MUST merge to a single view.
- **FR-011**: The system MUST show a single scene view during cutscenes, dialogue, loading, pause menus, and other non-gameplay states.
- **FR-012**: After a scene change, teleport, or gameplay resume, the system MUST re-evaluate the split state instead of keeping a stale split or merge.
- **FR-013**: Split and merge MUST be stable at the boundary; the display MUST NOT flicker when a character hovers near the transition.
- **FR-014**: Each player's view and controls MUST remain independent in split mode; one player's movement or camera MUST NOT take control of the other player's view.
- **FR-015**: The split view MUST remain at the target 60 frames per second in the supported test scene, with responsive input for both players.
- **FR-016**: The feature MUST NOT change the existing single-player experience or the co-op experience when the dog is absent.
- **FR-017**: The split/merge decision MUST be automatic; no manual toggle is required for this feature.
- **FR-018**: If either character or view is invalid or not initialized, the system MUST fall back to the single view rather than render a broken split.
- **FR-019**: The single UI MUST remain legible and anchored to the full screen in split mode; it MUST NOT be cropped, doubled, or attached to only one panel.
- **FR-020**: The feature MUST work in every scene where the dog can be present, without per-scene configuration.

### Key Entities *(include if data involved)*

- **Link (P1)**: The first player's character. Relevant state: position, camera view, input ownership, and whether Link is framed in the shared view.
- **Dog Companion (P2)**: The second player's controlled dog. Relevant state: present/active, position, camera view, input ownership, and whether the dog is framed in the shared view.
- **Shared View**: The single camera view used while both characters are framed together. Relevant state: which characters are inside the frame and how long they have been inside or outside it.
- **Split View**: The two scene views used when the characters are apart. Each view follows its own character; the UI is not part of the split.
- **Single UI**: The existing HUD and other on-screen information, shown once over the full screen in both single and split modes.
- **Split State**: The current display mode (single or split) and the short transition timers that prevent flicker.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With Link and the dog framed together for 30 seconds, 100% of sampled frames show one scene view and one UI.
- **SC-002**: In 10 controlled separation trials, the display reaches split mode within 0.5 seconds of the grace period expiring, in 100% of trials.
- **SC-003**: In 10 controlled reunion trials, the display returns to one view within 0.5 seconds of the grace period expiring, in 100% of trials.
- **SC-004**: With no dog present, a 60-second gameplay run shows zero split frames.
- **SC-005**: In 20 trials where P2 summons the dog beside Link while split, the display merges to one view in 100% of trials.
- **SC-006**: In a 30-second boundary-hover test, the display produces zero rapid split/merge oscillations.
- **SC-007**: In split mode, 100% of sampled frames contain exactly one UI instance; no HUD element is duplicated.
- **SC-008**: The split mode maintains the target 60 frames per second in the supported test scene, and both players' controls respond without noticeable delay.
- **SC-009**: During cutscenes, loading, and pause menus, 100% of sampled frames show a single scene view.
- **SC-010**: In split mode, each panel shows its own character within the frame in 100% of sampled frames.
- **SC-011**: After 10 scene changes or teleports, the split state is correct and not stale in 100% of cases.
- **SC-012**: Existing single-player and no-dog co-op verification scenarios pass without regression.

## Assumptions

- "Together" means both characters are framed in the shared view, not merely within a fixed world distance. "Apart" means one character has left that shared view.
- The grace period for leaving or returning is 0.5 seconds; this is the default used to prevent flicker and may be tuned during planning.
- The single view uses Link's camera, as the game does today; the dog remains visible when it is close enough to be framed.
- Split mode uses two equal scene panels, one per player. The default panel orientation is horizontal (top/bottom); the exact orientation is a presentation default that can be adjusted during planning without changing the behaviour.
- The UI remains the current single HUD overlay, anchored to the full screen. No per-player HUD, minimap, or dog-specific UI is introduced.
- Split mode is only active while the dog is present and P2 is controlling it. No dog, P2 disconnect, or an idle/non-active dog results in a single view.
- The feature is automatic; no settings toggle or manual camera switch is required.
- Cutscenes, dialogue, loading, pause menus, and other non-gameplay states always use a single scene view.
- The exact boundary may depend on camera framing and scene geometry, so the implementation may use a visibility/settle rule rather than a single fixed distance.
- The feature is expected to work across all dog-capable scenes without per-scene configuration.
- Maintaining 60 frames per second is required for the supported test scene; heavier scenes must degrade safely rather than leaving a broken or empty view.
- The first version replays the currently prepared scene/area. It does not load a second room solely for the dog view; if a second view cannot be prepared, the system falls back to one view.
- The existing co-op dog movement, rupee pickup, and other gameplay behaviour remain unchanged.
