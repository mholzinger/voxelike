# Voxelike Logo and Hero Transition

**Status** Prototype ready for playtesting in the 3D build

Use the finished Voxelike logo as the starting point for a continuous shot into the first dungeon. On Start, the camera rushes through the logo cubes, dives toward the actual spawn tile, and watches the equipped hero rise through the floor.

## Start and logo flythrough

Keep the current title assembly and readable logo as the setup. Once the finished title accepts Start, clear the prompt and version text and push the camera through the wordmark. The center of the word opens a path ahead while nearby cubes swell in perspective, streak past the camera, and leave through the edges of the screen. Let the last cubes clear before the dungeon floor becomes the focal point.

**Playtest decision** Does the movement feel like traveling through solid voxels? Try the flythrough first and review it at game size. If it reads as a flat logo growing until it crops, revisit the exit choreography with the alternate concept before committing to the shot.

## Dive to the spawn tile

Generate the first dungeon on the Start press that accepts the title. Use the real spawn tile and its floor height to frame the arrival. Bring the floor toward the camera, using tile edges and nearby walls to establish depth. Hold the close-up through the hero's entire rise. Once his boots meet the floor, hold the completed pose for a beat, then ease back to the playable view of the same room.

## Hero emergence

The equipped hero rises straight up through the spawn tile, as if an invisible elevator were carrying him. The head appears first, then the shoulders and weapon, then the boots. Clip the model against the actual floor plane so the body and gear emerge together according to their height. Ease to a stop when the boots meet the floor.

Use the weapon the scenario has equipped when the level is generated. The stock Rogue loadout starts with a mace in hand. Read the equipped slot through the port and use the same voxel gear model as gameplay so any scenario-specific weapon appears in the hero's hand from the moment it clears the floor.

## Playtest checks

- Pressing Start during the opening animation still reveals the finished title. A fresh press from the title begins the dungeon transition.
- At 320×224, the logo cubes visibly pass the viewer and the next shot picks up on the actual starting floor.
- The emergence reads in both an open room and a spawn near a wall. The hero's starting weapon is visible when its silhouette clears the floor and matches what the player holds on the first turn.
- At the handoff, the hero stands on the correct tile, the camera is settled, and entering the dungeon has spent zero Rogue turns.
