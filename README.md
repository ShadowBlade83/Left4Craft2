<div align="center">

# Left4Craft (Left 4 Dead 2 × Minecraft)

[![English](https://img.shields.io/badge/Language-English_🇬🇧-2ea44f?style=for-the-badge)](README.md)
[![Russian](https://img.shields.io/badge/Язык-Русский_🇷🇺-007ec6?style=for-the-badge)](README.ru.md)

</div>

Play Left 4 Dead 2 as Steve. Real Minecraft Java runs hidden next to it and handles the player's physics,
inventory, HUD and blocks, while L4D2 runs its own world, the infected, the Director and the campaigns. Build
barricades out of blocks right on L4D2's maps, and the infected have to break through them.

> A port of [HL2Craft](https://github.com/ShadowBlade83) (Half-Life 2 × Minecraft, by ShadowBlade83 / Maga) to
> Left 4 Dead 2.

## What works
- **Minecraft physics:** walking, sprinting, a 1.25-block jump, sneaking, pillaring up on your own blocks. The L4D2
  survivor is a puppet that stays at Steve's position (noclip, `setpos` every frame).
- **Blocks inside L4D2's frame:** Minecraft's section meshes are drawn with D3D9 at the end of L4D2's 3D scene, into
  the game's own depth buffer and with the game's own camera (the `cViewProj` matrix from vertex shader constants
  c8..c11). Blocks are hidden behind walls.
- **Minecraft's HUD instead of L4D2's:** Minecraft's hand, hotbar, hearts and screens are drawn over the frame
  (the ReShade effect `HL2Minecraft.fx`). A VScript removes L4D2's weapons and HUD.
- **Barricades:** a placed block becomes an `env_physics_blocker` in L4D2 with hit points by material (dirt 50,
  planks 150, cobblestone 300, iron 600, obsidian 2000). Nearby infected wear it down, a Tank fast, and a broken
  block breaks in Minecraft too.
- **Map collision:** L4D2's geometry around the player is scanned with traces and sent to Minecraft as
  1/8-block voxels.
- **Health:** Minecraft owns it. L4D2's hits reach Minecraft (÷5), and the survivor's health mirrors Minecraft's (×5).

## How it works
```
Left 4 Dead 2 (32-bit, D3D9)                         Minecraft 26.3 + Fabric (fabric)
  ReShade 6.8 + HL2Minecraft.addon32 (hl2)        mod hl2craft (Left4Craft)
    bridge: pose/look, input, collision scan  <-- shared memory -->  player physics, collision, blocks
    renderer: block meshes into L4D2's frame  <-- render ring  ---   section, atlas, entity export
    overlay: Minecraft's HUD/hand             <-- overlay      ---   HUD frame
  VScript left4craft.nut (L4D2's server)
    barricades, hit points, weapons/HUD, puppet
```
Scale: 1 block = 39.37 units (1 m). Axes: Source (x, y, z) → Minecraft (x, z + 64, −y). Each map gets its own region.

## Requirements
- Windows 10/11.
- **Left 4 Dead 2** (Steam). It runs with `-insecure`, which turns VAC off. **Local games only: never join
  official servers with the mod installed.** Before playing online, remove `d3d9.dll`, `HL2Minecraft.addon32` and
  `ReShade*.ini` from the game folder.
- **ReShade 6.8.0 with full add-on support** in L4D2's folder (`left4dead2.exe`, DirectX 9). The regular ReShade
  build does not load add-ons.
- **Minecraft Java 26.3** (your own account), Fabric Loader 0.19.5+, Fabric API for 26.3; JDK 25.
- To build the add-on: a 32-bit MinGW-w64 (`g++ -m32`, for example w64devkit).

## Build
```bat
cd hl2
build.bat
```
This builds `HL2Minecraft.addon32` and copies it into the mod's resources. Then:
```bat
cd fabric
gradlew.bat build
```
The mod jar ends up in `fabric/build/libs/`. Build the add-on first: the jar carries it, and it isn't kept in git.

Prebuilt `.jar` and `HL2Minecraft.addon32` are on the [Releases](../../releases) page.

## Run
- **Dev:** `gradlew.bat runClient` in `fabric`. Minecraft opens its mirror world, installs the add-on, the
  ReShade preset and `left4craft.nut` into L4D2's folder, and starts `left4dead2.exe -insecure -condebug` straight
  on `c1m1_hotel` (`-Dleft4craft.map` in `build.gradle`).
- **Normal:** put the jar into the `mods` folder of a Minecraft 26.3 instance (for example in Prism Launcher) and
  start Minecraft. The mod finds L4D2 through Steam, installs its files and launches the game.

## Controls (in L4D2's window)
| Key | Action |
|---|---|
| W A S D, Space, Shift, Ctrl | Minecraft movement: walk, jump, sneak, sprint |
| LMB / RMB | hit and break / place a block |
| 1–9, mouse wheel | hotbar slots |
| E | Minecraft inventory |
| Q, F, T, / | drop, swap to off hand, chat, command |
| F8 | debug: a slice of the collision around Steve's feet into `run.log` |

## Status and known issues
- Checked in game on c1m1: Minecraft physics, Minecraft's HUD, L4D2's weapons and HUD removed, blocks visible in
  the frame.
- Not checked in game yet: damage from the infected to Steve (just fixed), blocks on L4D2's camera without
  jitter, barricade hit points.
- Map collision is built from traces around the player, so getting stuck on stairs can still happen (F8 helps).
- Special infected (Hunter, Smoker…) don't get Minecraft stand-in mobs yet; fighting them is L4D2-only.
- Single player only (with bots) on your own server.

L4D2 specifics found along the way (method indices in the real `engine.dll`/`client.dll` differ from hl2sdk-l4d2,
`ClientCmd` filters commands, and so on) are recorded in the comments of
`hl2/src/hl2_bridge.h`.

## Layout
- `fabric/` — the Minecraft mod (Fabric 26.3, Java 25): link, physics hooks, collision, exporters; its resources
  carry the ReShade effect and `left4craft.nut` (the VScript side inside L4D2: barricades, weapons/HUD, puppet).
- `hl2/` — the 32-bit ReShade add-on (C++20): the L4D2 bridge, collision scan and D3D9 world renderer.
- `protocol/` — the shared memory layout both sides use (`hl2craft_protocol.h`).
- `shaders/` — the ReShade compositor effect `HL2Minecraft.fx`.

## Credits and licenses
- HL2Craft and this port: ShadowBlade83 (Maga). The code is under **CC BY-NC 4.0** (see
  `LICENSE`): non-commercial use, with credit to the author.
- Inspired by SkyCraft (chasmlol, Skyrim × Minecraft).
- ReShade API headers (`hl2/third_party/reshade`): crosire, BSD-3-Clause.
- Not affiliated with or endorsed by Valve, Mojang or Microsoft. No game files are stored in this repository.
