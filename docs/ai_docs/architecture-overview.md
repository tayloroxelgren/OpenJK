# OpenJK Architecture Overview

This document describes the high-level architecture of this repository as it is organized in source and build targets. OpenJK is a C/C++ engine project for Jedi Academy and parts of Jedi Outcast, descended from the Quake 3 engine family. The codebase is organized around separate single-player and multiplayer engine trees, dynamically loaded gameplay/UI/renderer modules, and shared platform/common support code.

## Top-Level Shape

| Path | Role |
| --- | --- |
| `CMakeLists.txt` | Top-level build orchestration, platform/architecture detection, binary naming, library selection, and subproject registration. |
| `code/` | Jedi Academy single-player tree: SP engine, server/client code, UI, game module, cgame code, renderer, ICARUS scripting, and SP-specific qcommon code. |
| `codemp/` | Jedi Academy multiplayer tree: MP client engine, dedicated server, server game module, client game module, UI module, renderers, botlib, networking, and MP-specific qcommon code. |
| `codeJK2/` | Optional Jedi Outcast single-player code paths and game module pieces. Built only when the JK2 CMake options are enabled. |
| `shared/` | Cross-tree support code: platform startup, dynamic library loading, SDL wrappers, console handling, shared math/string/version helpers, and safe utility wrappers. |
| `lib/` | Bundled third-party dependencies, including SDL2 headers/libs, OpenAL import libs, zlib, libpng, libjpeg, minizip, and gsl-lite. |
| `docs/` | Project notes for focused subsystems such as renderer architecture, save games, language strings, and library loading. |
| `tests/` | Optional unit tests, currently focused on safe utility code. |
| `cmake/` | Install modules and cross-compilation toolchains. |
| `scripts/`, `tools/` | Docker/runtime helper scripts and auxiliary tools such as the Windows symbol server utility. |

## Build Products

The top-level CMake file defines architecture-specific names such as `x86`, `x86_64`, `arm64`, or `i386`, then derives target names from that architecture. The important products are:

| Target family | Product | Source area | Purpose |
| --- | --- | --- | --- |
| SP engine | `openjk_sp.<arch>` | `code/`, `shared/` | Single-player executable. Hosts the client, local server, UI, filesystem, audio, input, and module loading. |
| SP game module | `jagame<arch>` | `code/game`, `code/cgame`, selected `code/ui` and qcommon files | Single-player gameplay and client-game logic. Loaded by the SP engine through the SP game API. |
| SP renderer | `rdsp-vanilla_<arch>` | `code/rd-vanilla`, `code/rd-common` | Loadable OpenGL renderer for single-player. |
| MP client | `openjk.<arch>` | `codemp/`, `shared/` | Multiplayer client executable. Hosts client, local server support, networking, audio, input, UI/cgame loading, and renderer loading. |
| MP dedicated server | `openjkded.<arch>` | `codemp/server`, `codemp/qcommon`, `codemp/rd-dedicated`, `codemp/null`, `shared/` | Headless multiplayer server with null client/audio/input paths and a dedicated renderer subset. |
| MP server game module | `jampgame<arch>` | `codemp/game` | Server-side multiplayer rules, entities, bots, combat, saber/force behavior, and game state. |
| MP client game module | `cgame<arch>` | `codemp/cgame` plus shared game pieces | Client-side prediction, player/entity presentation, effects, HUD/scoreboard, and rendering submissions. |
| MP UI module | `ui<arch>` | `codemp/ui` plus shared game pieces | Multiplayer menus, server browser UI, setup screens, and UI system calls. |
| MP renderers | `rd-vanilla_<arch>`, `rd-rend2_<arch>` | `codemp/rd-vanilla`, `codemp/rd-rend2`, `codemp/rd-common` | Default and experimental multiplayer renderers. |
| Bot library | `botlib` | `codemp/botlib` | Static library used by MP engine/dedicated server for AAS navigation, bot goals, chat, and movement. |

Most CMake options are switches around these products, for example `BuildSPEngine`, `BuildSPGame`, `BuildMPEngine`, `BuildMPGame`, `BuildMPCGame`, `BuildMPUI`, `BuildMPDed`, `BuildMPRdVanilla`, and `BuildMPRend2`.

## Runtime Layers

OpenJK follows the classic Quake 3 style split between a host executable and loadable modules.

```text
Platform / OS
  shared/sys + shared/sdl
    main(), SDL, console, input, dynamic library loading

Common engine
  qcommon
    cvars, commands, filesystem, networking/messages, collision model,
    memory zones/hunk, string tables, timing, VM/module support

Engine subsystems
  client, server, audio, renderer loader, UI/cgame/game API bridges

Loadable modules
  game, cgame, ui, renderer
    gameplay rules, client prediction/presentation, menus, rendering backend
```

The startup entry point is `shared/sys/sys_main.cpp`. Its `main()` builds the command line, initializes platform state, calls `Com_Init(...)`, and then repeatedly calls `Com_Frame()`. The common frame loop lives separately in `code/qcommon/common.cpp` for SP and `codemp/qcommon/common.cpp` for MP.

At a high level each frame does this:

```text
Sys main loop
  -> Com_Frame()
      -> process queued events and commands
      -> advance server frame when running
      -> advance client frame when running
      -> client submits scene/UI work to renderer
      -> renderer backend draws submitted work
```

## Engine and Module Boundaries

### Dynamic Loading

Dynamic loading is centralized in `shared/sys/sys_main.cpp` and `shared/sys/sys_loadlib.h`.

The project supports two module-call styles:

| Style | Entry points | Notes |
| --- | --- | --- |
| Legacy Quake 3 VM-style ABI | `vmMain(...)`, `dllEntry(...)` | Preserved for compatibility with existing mods. Modules call back into the engine through numbered `trap_*` syscalls. |
| OpenJK function-table ABI | `GetModuleAPI(...)` or SP `GetGameAPI(...)` | Type-safer import/export structs pass direct function pointers between engine and module. |

Multiplayer modules commonly expose both the newer `GetModuleAPI(...)` and legacy `vmMain(...)` entry points. Examples include:

- MP game: `codemp/game/g_main.c`
- MP cgame: `codemp/cgame/cg_main.c`
- MP UI: `codemp/ui/ui_main.c`

SP game loading uses `Sys_LoadSPGameDll(...)` and `GetGameAPI(...)`; MP game/cgame/UI use `Sys_LoadGameDll(...)` and `GetModuleAPI(...)`, with legacy fallback support where needed.

### Filesystem and Mod Search

The engine searches for loadable modules in the current mod directory from `fs_game`, then falls back to base locations. This is what keeps the executable stable while allowing mods to replace gameplay, UI, or client-game modules. `docs/libraries.md` has the focused explanation of this loading model.

## Single-Player Tree

`code/` contains the Jedi Academy single-player implementation.

Important areas:

- `code/client/`: SP client, input handling, console/screen code, cinematics, audio, and VM bridge code.
- `code/server/`: SP local server, save-game aware server flow, world/entity management, and game module loading.
- `code/qcommon/`: SP common layer for commands, cvars, filesystem, networking/messages, collision model, memory, save-game helpers, string tables, and shared definitions.
- `code/game/`: SP game rules, entities, NPC AI, weapons, saber/force behavior, vehicles, objectives, scripting bridge, save-game behavior, and server-side simulation.
- `code/cgame/`: SP client-game presentation, prediction, view/HUD, client effects, rendering submissions, and client-side entity handling. In the SP build this is bundled into the `jagame` module rather than built as a separate `cgame` library.
- `code/ui/`: SP menu/UI implementation and shared UI helpers.
- `code/rd-vanilla/`: SP OpenGL renderer implementation.
- `code/rd-common/`: Renderer-shared image/font/model/public renderer declarations.
- `code/icarus/`: ICARUS scripting support used by SP game logic.
- `code/ghoul2/`: Ghoul2 model/animation declarations used across game and renderer code.

Single-player is architecturally more monolithic than multiplayer: the executable contains the host engine and built-in UI/client/server systems, while the `jagame` module contains both game and SP cgame code.

## Multiplayer Tree

`codemp/` contains the Jedi Academy multiplayer implementation.

Important areas:

- `codemp/client/`: MP client, networking, UI/cgame API bridges, input, console/screen, cinematics, local effects, audio, and server browsing.
- `codemp/server/`: MP server, snapshots, client management, game module bridge, world state, challenge handling, bot integration, and networking.
- `codemp/qcommon/`: MP common layer for commands, cvars, filesystem, networking/messages, collision model, VM support, string tables, Roff system, memory, and shared definitions.
- `codemp/game/`: MP server-side rules, entities, player state, weapons, saber/force behavior, vehicles, bots, NPC pieces, and game syscalls.
- `codemp/cgame/`: MP client-side prediction, snapshots, player/entity presentation, HUD, effects, and renderer submissions.
- `codemp/ui/`: MP menus, server browser, settings UI, and UI syscalls.
- `codemp/botlib/`: Bot AAS routing, goals, movement, chat, character definitions, and supporting parsers/memory utilities.
- `codemp/rd-vanilla/`: Default MP OpenGL renderer.
- `codemp/rd-rend2/`: Experimental MP renderer with GLSL shaders and newer render pipeline pieces.
- `codemp/rd-dedicated/`: Renderer subset compiled into the dedicated server where model/collision-related renderer services are still needed.
- `codemp/null/`: Null client/input/sound/renderer stubs for dedicated-server builds.

Multiplayer keeps game, cgame, and UI as separate shared libraries. This is the main modding boundary for MP.

## Renderer Architecture

Renderer modules expose a renderer API to the engine and receive scene submissions from cgame/UI/client systems.

The renderer is split conceptually into:

- Front end: engine-facing API that receives scene data, entities, lights, polys, view definitions, and UI draw calls. It builds sorted render commands without issuing OpenGL calls directly.
- Back end: OpenGL-facing implementation that consumes queued render commands and draws the frame.

The focused renderer note is in `docs/renderer-architecture.md`. Source is split between renderer-specific directories (`rd-vanilla`, `rd-rend2`) and common renderer support (`rd-common`).

## Shared Infrastructure

`shared/` contains code used across SP, MP, and dedicated targets:

- `shared/sys/`: process startup/shutdown, console routing, crash/error behavior, library loading abstractions, platform-specific system calls, event pumping, and main loop glue.
- `shared/sdl/`: SDL-backed window, input, OpenGL, icon, and sound helpers.
- `shared/qcommon/`: small shared math, string, color, platform, version, and safe utility code used by both SP and MP trees.

The repository also has duplicated or parallel `qcommon` code under `code/` and `codemp/`. Those are not accidental: SP and MP have diverged enough that each tree carries its own common layer while still importing truly shared helpers from `shared/`.

## Data and Control Flow

### Client Rendering Flow

```text
Input/events
  -> client frame
      -> cgame prediction and presentation
          -> renderer front-end calls, e.g. add entities/lights/polys
              -> renderer command queue
                  -> renderer backend OpenGL draw
```

In MP, cgame is a separate module loaded by the client. In SP, cgame source is compiled into the SP game module.

### Server Gameplay Flow

```text
Com_Frame()
  -> SV_Frame(...)
      -> game module frame/game run functions
          -> entity simulation, player commands, AI, physics, combat
              -> server snapshots / local SP client state
```

The server owns authoritative gameplay state. MP serializes state to clients through snapshots. SP runs a local server/client model inside the single-player executable.

### Module Callback Flow

```text
Engine loads module
  -> module receives import table or legacy syscall pointer
      -> engine calls exported module functions
          -> module calls engine services through trap/import functions
```

Typical engine services exposed to modules include cvars, commands, filesystem, collision queries, renderer calls, sound calls, networking state, model/Ghoul2 operations, botlib services, and UI input state.

## External Dependencies

The build can use bundled or system libraries depending on platform and CMake options:

- SDL2 for windowing/input/platform integration.
- OpenAL for audio on supported builds.
- OpenGL for rendering.
- zlib, minizip, libpng, and libjpeg for compressed packages and image loading.
- gsl-lite for selected safe utility code.

Windows defaults to more bundled libraries. Some Apple and non-Windows builds prefer system libraries unless options such as `UseInternalSDL2`, `UseInternalZlib`, `UseInternalPNG`, or `UseInternalJPEG` are enabled.

## Where to Start When Changing Code

| Change area | Start here |
| --- | --- |
| Startup, main loop, platform behavior | `shared/sys/sys_main.cpp`, `shared/sys/sys_unix.cpp`, `shared/sys/sys_win32.cpp`, `shared/sdl/` |
| Commands, cvars, filesystem, memory, networking primitives | `code/qcommon/` or `codemp/qcommon/`, depending on SP vs MP |
| SP gameplay, AI, weapons, objectives, save-game behavior | `code/game/` |
| MP server gameplay, rules, bots, weapons, player state | `codemp/game/` |
| Client prediction, HUD, effects, entity presentation | `code/cgame/` for SP, `codemp/cgame/` for MP |
| Menus and UI | `code/ui/` for SP, `codemp/ui/` for MP |
| MP server behavior and snapshots | `codemp/server/` |
| SP server and save-game server flow | `code/server/` |
| Renderer behavior | `code/rd-vanilla/`, `codemp/rd-vanilla/`, `codemp/rd-rend2/`, and `*/rd-common/` |
| Bot AI library internals | `codemp/botlib/` |
| Build target composition | Top-level `CMakeLists.txt`, then `code/CMakeLists.txt` or `codemp/CMakeLists.txt` |

## Architectural Themes

- Compatibility is central. Legacy `vmMain`/`dllEntry` entry points and mod-directory search rules remain important compatibility surfaces.
- SP and MP are parallel but not identical. Do not assume a fix in `code/` automatically applies to `codemp/`, or vice versa.
- Engine/module ABI boundaries matter. Shared structs, syscall tables, import/export tables, and architecture-specific library names define what mods and binaries can exchange.
- Renderer modules are replaceable. The engine talks to renderers through a public renderer API, while renderer implementations own OpenGL details.
- The local-server model is still present. Even SP follows a client/server-ish flow internally, with the server side owning simulation and the client/cgame side owning presentation.
