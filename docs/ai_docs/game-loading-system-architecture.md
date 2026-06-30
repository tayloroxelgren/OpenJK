# Game Loading System Architecture

This document explains how OpenJK loads a level, its game state, and the assets needed to play it.

The short version is:

1. The filesystem decides where files come from: loose files, base game folders, mod folders, and `.pk3` archives.
2. The server starts a map through `SV_SpawnServer`.
3. The collision system loads the BSP for gameplay and traces.
4. The game DLL receives the map entity string and spawns gameplay entities.
5. The server publishes asset lists through configstrings.
6. The client/cgame side starts renderer, sound, UI, and cgame systems.
7. Cgame registers sounds, models, shaders, effects, skins, inline map models, and music.
8. The renderer loads the visual BSP world.
9. The game enters `SS_GAME`/`CA_PRIMED` and begins normal simulation/rendering.

In plain terms: loading is not one function. It is a coordinated handoff between filesystem, server, collision, game logic, client state, cgame, renderer, sound, effects, and savegame code.

## Main Source Areas

| Area | Important files | Role |
| --- | --- | --- |
| Filesystem startup | `code/qcommon/files.cpp` | Builds search paths and reads files from folders/PK3s. |
| Server map start | `code/server/sv_ccmds.cpp`, `code/server/sv_init.cpp` | Validates map commands and drives `SV_SpawnServer`. |
| Collision/BSP load | `code/qcommon/cm_load.cpp` | Loads BSP collision, entity string, visibility, brushes, patches, and area portals. |
| Game DLL load/init | `code/server/sv_game.cpp`, `code/game/g_main.cpp`, `code/game/g_spawn.cpp` | Loads game code, calls `InitGame`, and spawns entities from the map entity string. |
| Client loading state | `code/client/cl_main.cpp`, `code/client/cl_parse.cpp` | Flushes old client memory, starts renderer/sound/UI/cgame, parses gamestate. |
| Cgame level init | `code/cgame/cg_main.cpp` | Parses server info, loads collision map for cgame, registers sounds and graphics, starts music. |
| Renderer world load | `code/rd-vanilla/tr_bsp.cpp`, `code/rd-vanilla/tr_model.cpp` | Loads visual BSP lumps, lightmaps, shaders, surfaces, world effects, models. |
| Sound registration | `code/client/snd_dma.cpp`, `code/cgame/cg_main.cpp` | Starts sound system and registers level sounds. |
| Savegame load | `code/server/sv_savegame.cpp`, `code/game/g_savegame.cpp` | Opens `.sav`, spawns the map, then restores serialized game/level state. |
| Loading UI | `code/client/cl_scrn.cpp`, `code/cgame/cg_info.cpp`, `code/cgame/cg_main.cpp` | Shows load screen and progress strings while media is registered. |

This document focuses on the single-player `code/` path. Multiplayer has parallel pieces under `codemp/`.

## The Big Mental Model

The loading architecture has two overlapping jobs:

- **Load the world so the game can simulate it.**
- **Register the media so the player can see and hear it.**

Those are related, but not identical.

The server/collision/game side cares about:

- map name;
- BSP collision hulls;
- entity string;
- worldspawn;
- doors, triggers, NPCs, items, movers;
- configstrings;
- savegame state;
- baselines and snapshots.

The client/cgame/renderer/sound side cares about:

- visual BSP surfaces;
- lightmaps and light grid;
- shaders;
- models and skins;
- effects;
- sounds and music;
- HUD/load screen assets.

The same `.bsp` file feeds both sides, but each side reads different data from it.

## Filesystem Startup

Before a map can load, the filesystem must know where to search for files.

`FS_InitFilesystem` in `code/qcommon/files.cpp` runs during engine startup. It reads startup cvars such as:

- `fs_basepath`
- `fs_homepath`
- `fs_cdpath`
- `fs_game`
- `fs_basegame`
- `fs_dirbeforepak`

Then `FS_Startup(BASEGAME)` builds the search path. It adds game directories from base path, home path, optional base game, and optional mod game folders. It also scans `.pk3` archives and registers console commands like `path`, `dir`, `fdir`, `which`, and `fs_restart`.

The rest of the engine normally does not care whether a file came from a loose directory or a `.pk3`. It calls functions such as `FS_ReadFile`, `FS_FOpenFileRead`, and `FS_ListFiles`, and the filesystem resolves the actual source.

This matters for loading because nearly every later step uses this layer:

- map BSPs: `maps/<name>.bsp`
- shaders
- models
- skins
- sounds
- effects
- scripts
- UI menus
- savegame files

## Normal Map Load Flow

A normal map load starts from a command path such as `map <name>`.

The server command code checks that:

- the map name is valid;
- it does not contain path separators like `\`;
- `maps/<name>.bsp` exists.

Then it calls:

```cpp
SV_SpawnServer( map, eForceReload, qtrue );
```

`SV_SpawnServer` is the center of normal level loading.

## `SV_SpawnServer`: The Main Server Load

`SV_SpawnServer` in `code/server/sv_init.cpp` changes the game to a new map.

At a high level it does this:

1. Notifies the renderer/model system that a level load is beginning.
2. Resets pause/timescale state.
3. Shuts down the previous game DLL state.
4. Tells the client that map loading has begun.
5. Clears old per-level memory.
6. Loads collision data from the BSP.
7. Starts the game DLL and spawns entities.
8. Runs a few settle frames.
9. Creates baselines and reconnects the local client.
10. Publishes system/server configstrings.
11. Marks the server as in-game.

The important point is that the old level is deliberately torn down before the new one is built.

### Client-side flush during server load

Early in `SV_SpawnServer`, the server calls:

```cpp
CL_MapLoading();
```

That sounds client-specific, and it is. In single-player, the local server and local client live in the same process. Starting a new map needs to prepare the client too.

`CL_MapLoading`:

- closes the console;
- updates the load/connect screen;
- keeps or creates a localhost connection;
- calls `CL_FlushMemory`.

`CL_FlushMemory`:

- disables sounds;
- shuts down cgame;
- shuts down UI;
- shuts down the renderer without destroying the window/context;
- marks sound and renderer registration as invalid.

This prevents old cgame, renderer, UI, and sound data from leaking into the next level.

## Per-Level Memory Reset

OpenJK uses several memory systems. Loading a level intentionally clears large chunks of per-level memory.

Important reset steps in `SV_SpawnServer` include:

- freeing old snapshot entities;
- clearing the collision map if this is not the same map;
- resetting the server Ghoul2 miniheap;
- calling `Hunk_Clear`;
- clearing old configstrings;
- defragmenting cvar allocations;
- freeing old BSP/tagged data when collision reloads.

The hunk is important because many large level assets are allocated with the assumption that they live until the next level load. Instead of freeing every object one by one, the engine clears the per-level hunk and rebuilds the world.

## Collision BSP Load

The first major BSP load is for collision and gameplay:

```cpp
CM_LoadMap( va("maps/%s.bsp", server), qfalse, &checksum, qfalse );
```

This happens inside `SV_SpawnServer`.

`CM_LoadMap` in `code/qcommon/cm_load.cpp` loads the BSP into `clipMap_t`. It reads lumps such as:

- shaders/material info;
- leafs;
- leaf brushes;
- leaf surfaces;
- planes;
- brush sides;
- brushes;
- submodels;
- nodes;
- entity string;
- visibility;
- patch collision data.

This is the map as gameplay sees it. It powers:

- traces;
- player collision;
- entity collision;
- area portal state;
- brush models;
- collision for curved patches;
- access to the raw entity string.

It does not build the visible renderer world. That happens later.

### Cached BSP disk image

`CM_LoadMap` can keep a cached copy of the raw BSP disk image in `gpvCachedMapDiskImage` when memory allows.

The reason is practical: the server/collision system reads the BSP first, and the renderer will soon need the same file. Keeping the raw bytes around can let the renderer reuse the map data instead of reading the same BSP from disk again.

The renderer checks this cache in `RE_LoadWorldMap_Actual`.

## Game DLL Load and Entity Spawn

After collision loading, `SV_SpawnServer` sets:

```cpp
sv.state = SS_LOADING;
```

Then it calls:

```cpp
SV_InitGameProgs();
```

`SV_InitGameProgs` in `code/server/sv_game.cpp` loads the single-player game DLL:

- `jospgame` in JK2 mode;
- `jagame` otherwise.

It builds the import table that the game DLL can call back into, gets the exported game API, checks the API version, and hooks up the cgame VM entry point with `CL_InitCGameVM`.

Then it calls:

```cpp
ge->Init(
    sv_mapname->string,
    sv_spawntarget->string,
    sv_mapChecksum->integer,
    CM_EntityString(),
    sv.time,
    com_frameTime,
    Com_Milliseconds(),
    eSavedGameJustLoaded,
    qbLoadTransition );
```

That enters game code at `InitGame` in `code/game/g_main.cpp`.

`InitGame` prepares game globals, NPC systems, and level state, then calls:

```cpp
G_SpawnEntitiesFromString( entities );
```

`G_SpawnEntitiesFromString` in `code/game/g_spawn.cpp` parses the BSP entity string. The first entity must be `worldspawn`. After that, map entities are created from key/value pairs:

- triggers;
- doors;
- movers;
- NPC spawners;
- items;
- target entities;
- scripts;
- effects;
- misc models;
- sound emitters;
- brush entities.

This is where the static text stored in the BSP becomes actual gameplay objects.

## Configstrings: The Asset Manifest

During game initialization and entity spawning, the server fills configstrings.

Configstrings are indexed strings that describe shared state and media references. Examples include:

- `CS_SERVERINFO`
- `CS_SYSTEMINFO`
- `CS_SOUNDS`
- `CS_MODELS`
- `CS_ITEMS`
- `CS_CHARSKINS`
- `CS_BSP_MODELS`
- `CS_MESSAGE`

For loading, configstrings are effectively the server's manifest of things the client should know about.

For example:

- `CS_SOUNDS + i` tells cgame which sound names to register.
- `CS_MODELS + i` tells cgame which models the server referenced.
- `CS_ITEMS` tells cgame which item visuals/sounds matter for this map.
- `CS_BSP_MODELS + i` tells cgame about extra BSP instances.

The game code can request media indices during spawn. The client later reads those indices and registers the actual renderer/sound resources.

This is why media registration is split: the server decides what exists, but the client registers what it needs to draw and play.

## Settle Frames and Baselines

After game initialization, `SV_SpawnServer` runs several game frames:

```cpp
for ( i = 0 ; i < 4 ; i++ ) {
    ge->RunFrame( sv.time );
    sv.time += 100;
}
```

This gives spawned systems a chance to settle before the client begins normal play.

Then the server creates baselines with `SV_CreateBaseline`. Baselines are reference entity states used to compress future snapshots. The server also reconnects the local client if needed, sends the new gamestate, and eventually switches:

```cpp
sv.state = SS_GAME;
```

At that point, the server considers the level live.

## Client Hunk Users

The client-side systems that depend on per-level memory are restarted through:

```cpp
CL_StartHunkUsers();
```

This function is in `code/client/cl_main.cpp`.

It starts systems in a fixed order:

1. Renderer registration with `CL_InitRenderer`.
2. Sound startup with `S_Init`.
3. Sound registration reset with `S_BeginRegistration`.
4. UI startup with `CL_InitUI`.
5. Cgame startup with `CL_InitCGame`, once the client is connected enough.

The order matters. Cgame registration calls into renderer, sound, collision, and UI services. Those systems need to exist before cgame asks for media.

## Renderer Startup vs World Load

Renderer startup and world loading are separate.

`CL_InitRenderer` calls:

```cpp
re.BeginRegistration( &cls.glconfig );
```

That reaches `RE_BeginRegistration`, which calls `R_Init`. This initializes renderer internals:

- OpenGL state;
- images;
- shaders;
- skins;
- model system;
- world effects;
- fonts;
- renderer tables and backend data.

But this does not yet load the map's visual world.

The visual BSP is loaded later by cgame through:

```cpp
cgi_R_LoadWorldMap( cgs.mapname );
```

inside `CG_RegisterGraphics`.

## Cgame Level Initialization

`CL_InitCGame` finds the current map name from `CS_SERVERINFO`, stores `cl.mapname`, sets the client state to `CA_LOADING`, and calls:

```cpp
VM_Call( CG_INIT, clc.serverCommandSequence );
```

That enters `CG_Init` in `code/cgame/cg_main.cpp`.

`CG_Init`:

- registers core HUD/font/loading shaders;
- loads HUD menus;
- opens the load screen;
- clears client entity state;
- transitions permanent entities;
- calls `CG_GameStateReceived`.

`CG_GameStateReceived` is where the cgame side performs most map media loading.

It does this:

1. Reads renderer config with `cgi_GetGlconfig`.
2. Gets the gamestate with `cgi_GetGameState`.
3. Parses server info.
4. Registers the level-load shader.
5. Loads the collision map on the client side with `cgi_CM_LoadMap`.
6. Registers sounds with `CG_RegisterSounds`.
7. Registers graphics with `CG_RegisterGraphics`.
8. Starts music with `CG_StartMusic`.

The client collision load usually hits the already-loaded map path and is much cheaper than the server's initial collision load, but cgame still calls through the same collision API so it can query inline models and map collision information.

## Sound Registration During Load

Before cgame registers sounds, the client calls:

```cpp
S_BeginRegistration();
```

The comment says all old sound handles become invalid. This is the sound system's level boundary.

Then `CG_RegisterSounds` registers sounds needed by the level:

- general player/interface sounds;
- footsteps;
- force/weapon feedback sounds;
- item sounds listed by `CS_ITEMS`;
- sounds listed by `CS_SOUNDS`;
- ambient sound sets through `CG_AS_Register`.

Registered sounds become `sfxHandle_t` values and are stored in structures such as `cgs.media` and `cgs.sound_precache`.

This is why gameplay code can later use a small sound index instead of doing a filesystem lookup during combat.

## Graphics and Media Registration

`CG_RegisterGraphics` is the largest media registration stage.

It registers or loads:

- effects;
- the visual world map;
- HUD and number shaders;
- global game shaders;
- common models;
- item visuals;
- wall mark shaders;
- inline brush models;
- server-listed models from `CS_MODELS`;
- skins from `CS_CHARSKINS`;
- player/NPC client models;
- NPC custom sounds/effects;
- static misc entities;
- sub-BSP instances from `CS_BSP_MODELS`;
- terrain/world effect support.

The visual BSP load happens here:

```cpp
cgi_R_LoadWorldMap( cgs.mapname );
```

That reaches `RE_LoadWorldMap` in `code/rd-vanilla/tr_bsp.cpp`.

## Visual BSP Load

The renderer's `RE_LoadWorldMap_Actual` reads the same BSP file but builds renderer-facing data.

It loads lumps such as:

- shaders;
- lightmaps;
- planes;
- fog volumes;
- draw surfaces;
- draw vertices and indexes;
- marksurfaces;
- render nodes and leafs;
- submodels;
- visibility;
- worldspawn render keys;
- light grid;
- light grid array.

This is the map as the renderer sees it. It powers:

- world surfaces;
- lightmaps;
- static lighting data;
- fog;
- visibility;
- renderable inline models;
- sky/sun/worldspawn visual settings;
- light-grid lookup for dynamic entities.

The renderer and collision loaders read some overlapping BSP data, but their outputs are different structures. Collision builds `clipMap_t`; rendering builds `world_t`.

## Sub-BSPs and Inline Models

The engine also supports BSP instances/sub-BSPs.

Cgame reads `CS_BSP_MODELS`. For each listed BSP instance, it calls:

```cpp
cgi_CM_LoadMap( bspName, qtrue );
cgi_R_RegisterModel( bspName );
```

The collision side loads the sub-BSP data, and the renderer registers the visual model. Additional inline model names use forms like:

```text
*<subBspIndex>-<subModelIndex>
```

This is more complicated than ordinary MD3/GLM model loading because the "model" is another BSP with its own internal submodels.

## End of Registration

After cgame initialization, `CL_InitCGame` calls:

```cpp
re.EndRegistration();
Com_TouchMemory();
```

`re.EndRegistration` lets the renderer finish the media registration phase. Renderer backends often use this point to touch images/resources so they are resident before gameplay begins.

`Com_TouchMemory` walks memory to page things in. The goal is to reduce stalls right after the load screen disappears.

At the end of this path, the client state moves toward `CA_PRIMED`: it is ready to send a usercmd and receive the first real snapshot.

## Savegame Loading

Savegame loading uses the normal map loading path, then overlays serialized state.

The main function is:

```cpp
SG_ReadSavegame( const char *psPathlessBaseName )
```

in `code/server/sv_savegame.cpp`.

It opens the savegame, then reads chunks such as:

- comment text;
- screenshot data;
- map command/map name;
- saved cvars;
- game-level metadata;
- autosave/full-save flag.

Then it calls:

```cpp
SV_SpawnServer( sMapCmd, eForceReload_NOTHING, ... );
```

This is important: even loading a savegame first spawns the target map normally. The engine needs the map, collision, game DLL, entity definitions, renderer, and media systems set up before it can restore detailed state.

After `SV_SpawnServer`, full savegames restore additional server-level data:

- `sv.time`;
- `sv.timeResidual`;
- collision area portal state;
- server configstrings.

Finally the game DLL reads level/entity state:

```cpp
ge->ReadLevel( qbAutosave, qbLoadTransition );
```

That reaches `ReadLevel` in `code/game/g_savegame.cpp`, which restores:

- player/client state for full saves;
- level locals;
- objectives;
- effects;
- entities;
- scripted variables;
- miscellaneous game data;
- selected cgame HUD/player state hacks used by the save system.

Autosaves are lighter. `ReadGame` restores some player information into cvars such as player save data, ammo, inventory, and force power levels before the map spawn finishes.

## Why Savegames Load the Map First

A savegame is not a complete standalone copy of every asset and engine structure. It depends on the map and game code still being available.

The save file records state like:

- what map to load;
- which entities exist and their fields;
- current time;
- cvars;
- portal state;
- objective data;
- script state;
- player data.

It does not replace the need to load:

- the BSP file;
- collision structures;
- renderer world;
- shaders;
- models;
- sounds;
- effects;
- game DLL code.

So savegame loading is:

```text
open save
  -> read map name and saved metadata
  -> normal map spawn/load
  -> restore serialized state into the live map
```

not:

```text
open save
  -> skip map load
  -> magically resume everything
```

## Loading Screen and Progress Text

The load screen is updated from several places.

`CL_MapLoading` and `SCR_UpdateScreen` get an early loading/connect screen visible. Later, cgame opens the `"loadscreen"` UI menu and calls `CG_LoadingString` with labels such as:

- `collision map`
- `general sounds`
- `item sounds`
- `preregistered sounds`
- map name;
- `game media shaders`
- `game media models`
- `map brushes`
- `map models`
- `skins`
- `static models`
- `BSP instances`
- `music`

These strings are not separate loading systems. They are status updates emitted while the real registration functions run.

## What "Precache" Means Here

In this codebase, "precache" usually means:

- resolve a name to an engine handle during loading;
- load or prepare the underlying asset enough that gameplay can reference it cheaply;
- store that handle in `cgs.media`, `cgs.sound_precache`, `cgs.model_draw`, item data, weapon data, NPC data, or similar arrays.

It does not always mean "fully uploaded to GPU and guaranteed never to stall." For renderer assets, `re.EndRegistration` helps finalize/touch registered resources, but the old renderer still has several layers of lazy and backend-specific behavior.

## Normal Map Load Call Stack Summary

The typical happy path looks like this:

```text
map command
  -> SV_Map / command handler
  -> SV_SpawnServer
    -> re.RegisterMedia_LevelLoadBegin
    -> SV_ShutdownGameProgs
    -> CL_MapLoading
      -> CL_FlushMemory
    -> Hunk_Clear / configstring clear / cvar defrag
    -> CM_LoadMap                    collision BSP
    -> SV_InitGameProgs
      -> load game DLL
      -> CL_InitCGameVM
      -> ge->Init
        -> InitGame
        -> G_SpawnEntitiesFromString
    -> run settle frames
    -> SV_CreateBaseline
    -> sv.state = SS_GAME
  -> client receives/parses gamestate
  -> CL_StartHunkUsers
    -> CL_InitRenderer
      -> re.BeginRegistration
      -> R_Init
    -> S_Init
    -> S_BeginRegistration
    -> CL_InitUI
    -> CL_InitCGame
      -> CG_Init
      -> CG_GameStateReceived
        -> cgi_CM_LoadMap             client collision access
        -> CG_RegisterSounds
        -> CG_RegisterGraphics
          -> cgi_R_LoadWorldMap       visual BSP
          -> register shaders/models/effects/skins/items/NPCs
        -> CG_StartMusic
      -> re.EndRegistration
      -> Com_TouchMemory
```

## Practical Debugging Guide

If loading fails before the map appears:

- Check the filesystem path with `path` or `which`.
- Confirm `maps/<name>.bsp` exists.
- Check `CM_LoadMap` errors for bad/missing BSP or wrong BSP version.
- Check `SV_InitGameProgs` errors for game DLL load/API mismatch.
- Check `SP_worldspawn`/entity parse errors if entity spawn fails.
- Check configstrings if a client asset is missing but the server thinks it exists.
- Check `CG_RegisterSounds` for missing/invalid sound names.
- Check `CG_RegisterGraphics` for missing models, skins, shaders, effects, or sub-BSPs.
- Check `RE_LoadWorldMap` for visual BSP load/version errors.
- For savegames, check the saved map command and savegame version/chunk read errors first.

## Common Pitfalls

### Collision loaded but the screen is still blank

Collision BSP loading is not the same as renderer world loading. `CM_LoadMap` can succeed while `RE_LoadWorldMap` or later media registration fails.

### A model or sound works in gameplay code but not after loading

The server may have created an index/configstring, but cgame still needs to register the client-side asset. Look at the relevant `CS_*` configstring and the cgame registration function.

### Savegame loads the wrong-looking state

Savegame loading first spawns the map normally, then restores serialized state. If the map's entity definitions, scripts, or assets changed in incompatible ways, the restored state may no longer match the newly spawned baseline.

### Reloading the same map behaves differently

`CM_SameMap`, cached BSP data, and savegame/respawn paths can reuse or partially preserve some map data. That is intentional, but it means same-map reloads are not always identical to cold loads.

### Loading hitches after the load screen

Registration tries to prepare media up front, and `Com_TouchMemory` tries to page memory in, but the old renderer/sound architecture can still defer some work. A newly encountered shader path, model variation, sound, or effect may still create a hitch if it was not registered during the loading pass.

## Where to Start for Changes

- Map command validation: `code/server/sv_ccmds.cpp`.
- Main map load order: `SV_SpawnServer` in `code/server/sv_init.cpp`.
- Collision BSP loading: `CM_LoadMap` in `code/qcommon/cm_load.cpp`.
- Game DLL startup: `SV_InitGameProgs` in `code/server/sv_game.cpp`.
- Entity spawning: `InitGame` in `code/game/g_main.cpp` and `G_SpawnEntitiesFromString` in `code/game/g_spawn.cpp`.
- Client subsystem startup: `CL_StartHunkUsers` in `code/client/cl_main.cpp`.
- Cgame loading/media registration: `CG_Init`, `CG_GameStateReceived`, `CG_RegisterSounds`, and `CG_RegisterGraphics` in `code/cgame/cg_main.cpp`.
- Visual BSP loading: `RE_LoadWorldMap` in `code/rd-vanilla/tr_bsp.cpp`.
- Renderer registration boundaries: `RE_BeginRegistration` and `RE_EndRegistration` in `code/rd-vanilla/tr_model.cpp` / `code/rd-vanilla/tr_init.cpp`.
- Savegame loading: `SG_ReadSavegame` in `code/server/sv_savegame.cpp` and `ReadLevel` in `code/game/g_savegame.cpp`.
