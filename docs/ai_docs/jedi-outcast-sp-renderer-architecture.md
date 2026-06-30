# Jedi Outcast Single-Player Renderer Architecture

This document explains the renderer architecture for Jedi Outcast single player as implemented in OpenJK. It is intentionally scoped to the JK2 single-player path only. It does not describe Jedi Academy multiplayer renderers, the MP `rend2` renderer, or the MP dedicated renderer.

The short version: Jedi Outcast SP uses the single-player "vanilla" OpenGL renderer from `code/rd-vanilla/`, built with `JK2_MODE` enabled and loaded by the JK2 SP executable as `rdjosp-vanilla_<arch>`.

## What "Renderer" Means Here

In this codebase, the renderer is the part of the engine that turns game state into pixels. It is responsible for:

- Opening and managing the OpenGL-backed game window.
- Loading textures, shaders, models, fonts, and world geometry.
- Receiving "things to draw" from the client/game side.
- Figuring out which surfaces are visible from the camera.
- Sorting surfaces so transparency, fog, portals, and shader effects draw in the right order.
- Sending OpenGL commands that draw the final frame.

The renderer does not decide game rules. It does not decide enemy behavior, weapon damage, puzzle state, or save-game logic. It receives a view of the world and draws it.

## The JK2 SP Renderer Target

Jedi Outcast single player is controlled by these CMake options and names:

| Item | Value |
| --- | --- |
| Engine option | `BuildJK2SPEngine` |
| Game option | `BuildJK2SPGame` |
| Renderer option | `BuildJK2SPRdVanilla` |
| Engine binary name | `openjo_sp.<arch>` |
| Renderer binary name | `rdjosp-vanilla_<arch>` |
| Main renderer source | `code/rd-vanilla/` |
| Shared renderer source | `code/rd-common/` |
| JK2 SP source | `codeJK2/` |

There is no separate `codeJK2/rd-vanilla/` renderer tree. Instead, the shared SP renderer in `code/rd-vanilla/` is compiled a second way with `JK2_MODE` defined. That build flag enables Jedi Outcast-specific behavior where the renderer needs to differ from Jedi Academy SP.

The relevant build wiring is in:

- `CMakeLists.txt`: defines `BuildJK2SPRdVanilla` and the `rdjosp-vanilla_<arch>` name.
- `code/rd-vanilla/CMakeLists.txt`: builds the renderer library and adds `JK2_MODE` for the JK2 SP renderer target.
- `code/CMakeLists.txt`: builds the SP executable path and adds `JK2_MODE` when building `openjo_sp.<arch>`.

## How the Engine Loads the Renderer

The JK2 SP executable starts like the rest of the engine: platform code enters the common engine loop, then the client initializes the renderer.

The important client-side function is `CL_InitRef()` in `code/client/cl_main.cpp`.

In JK2 mode, the default renderer name is:

```text
rdjosp-vanilla
```

At runtime the engine appends the architecture and shared-library extension. For example, on a 64-bit Linux-style build, the loaded filename follows this pattern:

```text
rdjosp-vanilla_x86_64.so
```

The loading process is:

```text
openjo_sp
  -> CL_InitRef()
      -> choose cl_renderer, defaulting to rdjosp-vanilla
      -> Sys_LoadDll(...)
      -> find exported GetRefAPI(...)
      -> pass engine services to renderer
      -> receive renderer function table back
      -> store it in global refexport_t re
```

That `re` table is how the rest of the SP client/server code talks to the renderer. For example, code calls `re.RegisterModel`, `re.LoadWorld`, `re.RenderScene`, or `re.EndFrame` without needing to know which renderer library supplied those functions.

## The Renderer API Boundary

The API contract lives mostly in `code/rd-common/tr_public.h`.

There are two key structs:

| Struct | Direction | Meaning |
| --- | --- | --- |
| `refimport_t` | Engine to renderer | Functions the renderer can call back into: filesystem, cvars, commands, memory allocation, window setup, OpenGL function lookup, collision traces, save-game helpers, sound/cinematic helpers, and timing. |
| `refexport_t` | Renderer to engine | Functions the renderer exposes: initialize/shutdown, register media, load world, draw scenes, draw 2D images/text, get screenshots, access Ghoul2 model APIs, and frame begin/end calls. |

Think of this as a plug socket:

```text
Engine gives renderer tools:
  "Here is how you read files, allocate memory, open a window, ask for cvars,
   get OpenGL functions, and report errors."

Renderer gives engine drawing controls:
  "Here is how you load a map, register models, add objects to a scene,
   draw the scene, draw UI images, and finish a frame."
```

The renderer library exports one real linker symbol:

```text
GetRefAPI(int apiVersion, refimport_t *refimp)
```

`GetRefAPI()` checks the API version, stores the import table, fills a static `refexport_t`, and returns it to the engine.

## Major Renderer Source Areas

| Path/File | Role |
| --- | --- |
| `code/rd-vanilla/tr_init.cpp` | Renderer startup/shutdown, cvars, OpenGL extension setup, `GetRefAPI()`, command registration, global renderer initialization. |
| `code/rd-vanilla/tr_cmds.cpp` | Frame begin/end, render command buffering, screenshot and 2D draw command submission. |
| `code/rd-vanilla/tr_scene.cpp` | Scene collection: clear scene, add entities, add lights, add polygons, accept `refdef_t`, and start rendering a view. |
| `code/rd-vanilla/tr_main.cpp` | Core view setup, draw-surface generation, sorting, projection, frustum behavior, mirror/portal handling. |
| `code/rd-vanilla/tr_backend.cpp` | OpenGL backend: consumes sorted draw commands and emits the actual OpenGL drawing work. |
| `code/rd-vanilla/tr_world.cpp` | World visibility: PVS checks, area masks, frustum culling, recursive BSP traversal, adding map surfaces. |
| `code/rd-vanilla/tr_bsp.cpp` | BSP map loading: reads world/map data, lightmaps, planes, nodes, leaves, surfaces, fog, submodels. |
| `code/rd-vanilla/tr_model.cpp` | Model registration and model-cache behavior for brush, MD3, and Ghoul2-related model paths. |
| `code/rd-vanilla/tr_mesh.cpp` | MD3 mesh surface submission and related model drawing setup. |
| `code/rd-vanilla/tr_ghoul2.cpp`, `G2_*.cpp` | Ghoul2 skeletal model loading, animation, bolts, bone transforms, surfaces, ragdoll-related rendering hooks. |
| `code/rd-vanilla/tr_shader.cpp` | Shader script parsing, shader stage setup, sort order, texture coordinate mods, blend/depth behavior. |
| `code/rd-vanilla/tr_image.cpp` | Texture/image registration, mipmaps, texture filtering, upload to OpenGL. |
| `code/rd-common/tr_image_*.cpp` | JPG/PNG/TGA loading and shared image helpers. |
| `code/rd-common/tr_font.cpp` | Font drawing, glyph handling, and JK2/JKA language differences. |
| `code/rd-common/tr_public.h` | Public renderer API shared with the engine. |
| `code/rd-common/tr_types.h` | Public render data types such as `refEntity_t`, `refdef_t`, `glconfig_t`, draw flags, and render entity types. |

## The Frame in Plain English

Every visible frame goes through two broad halves:

1. The front end collects what should be drawn.
2. The back end actually talks to OpenGL.

The split matters because collecting and sorting a scene is different work from drawing triangles. The front end thinks in game-engine objects. The back end thinks in GPU state, textures, shaders, batches, and draw calls.

Here is the flow:

```text
Client/game code prepares a view
  -> re.BeginFrame(...)
  -> re.ClearScene()
  -> re.AddRefEntityToScene(...) for models/sprites/beams/etc.
  -> re.AddLightToScene(...) for dynamic lights
  -> re.AddPolyToScene(...) for particles and simple custom polygons
  -> re.RenderScene(refdef)
      -> renderer figures out visible world surfaces
      -> renderer adds entity/model surfaces
      -> renderer sorts draw surfaces
      -> renderer queues backend draw commands
  -> 2D UI/cinematic/text drawing as needed
  -> re.EndFrame(...)
      -> backend executes commands
      -> window presents final image
```

A layman analogy: the front end is like making a carefully ordered shot list for a film scene. The back end is the camera crew actually filming each shot with the right lens, lighting, and props.

## Key Data Objects

### `refdef_t`: The Camera Request

Defined in `code/rd-common/tr_types.h`, `refdef_t` tells the renderer what camera view to draw.

It includes:

- Screen rectangle: `x`, `y`, `width`, `height`.
- Field of view: `fov_x`, `fov_y`.
- Camera position: `vieworg`.
- Camera orientation: `viewaxis`.
- Current render time: `time`.
- Area visibility mask: `areamask`.
- Render flags: `rdflags`, such as no-world-model or special vision effects.

In plain terms, `refdef_t` says:

```text
"Draw the world from this position, looking this way, into this part of the screen."
```

### `refEntity_t`: One Thing to Draw

Also defined in `tr_types.h`, `refEntity_t` represents a drawable object.

It can be:

- A model.
- A sprite.
- A beam or line.
- An electricity effect.
- A saber glow.
- A portal marker.
- A custom polygon-like object.

It carries position, orientation, animation frames, shader overrides, render flags, scale, and Ghoul2 model pointers.

In plain terms:

```text
"Draw this object here, rotated this way, using this model/shader/animation state."
```

### `drawSurf_t`: One Surface Ready for Sorting

The renderer breaks worlds and models into surfaces. Each surface gets paired with a shader and packed sort key. That pair is a draw surface.

The renderer sorts draw surfaces so that:

- Opaque things draw efficiently.
- Portals and mirrors can trigger another view first.
- Fog is grouped correctly.
- Transparent and blended effects draw later.
- Shader state changes are reduced.

## World Rendering

Jedi Outcast maps are BSP worlds. A BSP map is not just a pile of triangles; it is spatially organized so the renderer can quickly skip large parts of the level.

The world-rendering flow is:

```text
re.LoadWorld("maps/....bsp")
  -> tr_bsp.cpp reads map lumps
  -> world nodes/leaves/surfaces/lightmaps/fog are stored

Each scene:
  -> R_AddWorldSurfaces()
      -> R_MarkLeaves()
          -> use PVS and areamask to find potentially visible leaves
      -> R_RecursiveWorldNode()
          -> walk visible BSP nodes
          -> frustum-cull boxes outside the camera
          -> add visible map surfaces as drawSurfs
```

Important ideas:

- PVS means "potentially visible set." It is precomputed visibility data from the map. If two areas cannot possibly see each other, the renderer can skip work.
- The areamask handles doors and area portals. If a door closes, the renderer can stop drawing the area behind it.
- Frustum culling removes objects outside the camera cone.
- Lightmaps are baked lighting textures loaded from the BSP and combined with surface textures during rendering.

## Lighting System

Jedi Outcast SP uses an older, mostly precomputed lighting model. It is not a modern fully dynamic lighting engine where every lamp lights every object in real time. Instead, most lighting is baked into the map ahead of time, and the renderer adds a smaller amount of real-time lighting for moving effects and models.

There are four main lighting pieces:

| Lighting piece | What it lights | Where it comes from | Main code |
| --- | --- | --- | --- |
| Lightmaps | Static world surfaces such as walls, floors, and ceilings | Baked into the BSP map by the map compiler | `code/rd-vanilla/tr_bsp.cpp`, `code/rd-vanilla/tr_shader.cpp`, `code/rd-vanilla/tr_shade.cpp` |
| Light grid | Moving entities such as characters and items | Baked 3D samples stored in the BSP | `code/rd-vanilla/tr_light.cpp` |
| Dynamic lights | Temporary real-time glows from weapons, effects, powerups, and scripted lights | Submitted by cgame/game code each frame | `code/rd-vanilla/tr_scene.cpp`, `code/rd-vanilla/tr_light.cpp` |
| Light styles | Flicker/pulse/toggle color changes applied to styled map lighting | Game/cgame config strings and renderer style tables | `code/cgame/cg_lights.cpp`, `code/rd-vanilla/tr_init.cpp`, `code/rd-vanilla/tr_shade.cpp` |

### Static World Lighting: Lightmaps

Most walls and floors are lit by lightmaps. A lightmap is a low-resolution texture that stores precomputed brightness and color. The game combines that lightmap with the normal surface texture.

In plain terms:

```text
wall texture:
  "This is a stone wall."

lightmap:
  "The left side is dark, the right side is orange from a lamp."

final result:
  "A stone wall with baked shadows and colored light."
```

The renderer loads lightmaps in `R_LoadLightmaps()` in `tr_bsp.cpp`. The BSP stores them as 24-bit RGB blocks. The renderer expands them to 32-bit RGBA textures, applies overbright/color shifting through `R_ColorShiftLightingBytes()`, then uploads them as OpenGL textures.

When shader scripts ask for `$lightmap`, `tr_shader.cpp` connects a surface's material to the correct baked lightmap texture. Later, backend shader drawing in `tr_shade.cpp` blends the base texture and lightmap stages.

This is why static rooms can have rich shadowing without the renderer doing expensive real-time light calculations for every wall every frame.

### Entity Lighting: The Light Grid

Moving objects cannot use wall lightmaps directly. A character can walk through the room, so the renderer needs to estimate how bright that character should be at the character's current position.

The BSP contains a 3D light grid: a set of baked lighting samples throughout the playable space. Each sample stores:

- Ambient light: general light from all directions.
- Directed light: stronger light coming from a direction.
- Direction: encoded as latitude/longitude bytes.
- Optional light styles.

When a model is drawn, `R_SetupEntityLighting()` calls `R_SetupEntityLightingGrid()` in `tr_light.cpp`. The renderer:

1. Chooses the entity's lighting point, usually `origin`, or `lightingOrigin` when `RF_LIGHTING_ORIGIN` is set.
2. Converts that world position into light-grid coordinates.
3. Samples the eight surrounding grid points.
4. Blends those samples together, a process called trilinear interpolation.
5. Applies active light-style colors.
6. Applies `r_ambientScale` and `r_directedScale`.
7. Stores the result on the render entity as ambient light, directed light, and light direction.

In layman terms, the renderer asks:

```text
"The player is standing between these eight invisible lighting probes.
 Blend those probes together and use that as the player's lighting."
```

This is what lets a moving character become darker in a hallway and brighter near a lit area, even though the character itself was not present when the map was compiled.

### Dynamic Lights

Dynamic lights are temporary real-time lights. Examples include weapon fire, explosions, glowing pickups, force effects, and scripted `misc_dlight` entities.

The client/game side submits them with:

```text
re.AddLightToScene(origin, radius, red, green, blue)
```

The renderer stores them in the current frame's dynamic-light list in `RE_AddLightToScene()` in `tr_scene.cpp`. The maximum is limited by `MAX_DLIGHTS`, which is 32.

Dynamic lights affect rendering in two related ways:

- For moving entities, `R_SetupEntityLighting()` adds nearby dynamic lights to the entity's directed light.
- For world and brush surfaces, the renderer tracks which dynamic lights might affect a surface using bit masks, then the backend can run extra lighting work for those marked surfaces.

The falloff is distance-based. `tr_light.cpp` treats each dynamic light as having a radius and color, then reduces its effect as the entity gets farther away. Very close distances are clamped so the math does not explode into huge light values.

Dynamic lights can be disabled globally by `r_dynamiclight 0`. They are also suppressed when `r_vertexLight` is enabled, because that mode intentionally avoids lightmap/dynamic-light style rendering.

### Light Styles

Light styles are how the engine changes baked lighting over time. A light style can make lights flicker, pulse, dim, brighten, or toggle.

The game side drives style changes. For example, `misc_dlight` and scripted lighting behavior can update config strings. Client code in `code/cgame/cg_lights.cpp` runs those styles and calls:

```text
trap_R_SetLightStyle(styleIndex, packedColor)
```

The renderer stores style colors in `styleColors` in `tr_shade.cpp`. `RE_SetLightStyle()` in `tr_init.cpp` updates the table and marks a style as changed.

Light styles are applied in two places:

- Light-grid sampling multiplies ambient and directed entity lighting by the active style color.
- Shader/lightmap stages can carry style data, so styled lightmaps can change appearance without rebuilding the map.

In practical terms, this is how a precomputed light can still appear to flicker in-game.

### Fog as Part of Lighting Atmosphere

Fog is not technically "lighting," but it strongly affects how lit a scene feels. JK2 SP stores fog data in BSP fog volumes and shader `fogParms`. The renderer assigns world surfaces, models, and polygons to fog volumes, then backend fog passes blend surfaces toward the fog color based on distance and volume rules.

Relevant files are:

- `tr_bsp.cpp`: loads fog definitions from the map.
- `tr_shader.cpp`: parses `fogParms` and fog behavior.
- `tr_scene.cpp`, `tr_mesh.cpp`, `tr_ghoul2.cpp`: choose which fog volume a submitted surface/entity belongs to.
- `tr_shade_calc.cpp` and backend shading paths: compute fog coordinates and apply fog passes.

### How Lighting Fits Into One Frame

The lighting path during a normal scene looks like this:

```text
Map load:
  -> load BSP lightmaps
  -> load BSP light grid
  -> load fog volumes and shader fog parameters

Each frame:
  -> cgame/game submits dynamic lights
  -> renderer gathers visible world surfaces
  -> world surfaces keep their baked lightmap/shader data
  -> renderer samples light grid for each lit model
  -> renderer adds nearby dynamic lights to model lighting
  -> renderer sorts surfaces by shader, fog, entity, and dynamic-light bits
  -> backend draws base textures, lightmaps, dynamic-light passes, glow/fog passes
```

The important takeaway: world lighting is mostly texture-based and precomputed; model lighting is sampled from a baked 3D grid; dynamic lights are short-lived additions layered on top.

## Shadow System

Jedi Outcast SP shadows are not modern shadow maps. The renderer does not render the whole scene from every light's point of view into depth textures. Instead, it uses older techniques that are cheaper and fit the Quake 3-era renderer:

- Simple ground marks for basic blob shadows.
- Stencil-buffer shadow volumes for higher-quality model silhouettes.
- Planar projected shadows for flattened model shadows on a ground plane.

The main user-facing control is `cg_shadows`. In the renderer, this is read as `r_shadows` from the same `cg_shadows` cvar.

| `cg_shadows` value | Shadow mode | Plain-English meaning | Main code |
| --- | --- | --- | --- |
| `0` | Off | Do not draw gameplay shadows. | `code/cgame/cg_players.cpp` |
| `1` | Mark/blob shadow | Trace to the floor and place a fading dark decal under the character. | `code/cgame/cg_players.cpp`, `code/cgame/cg_marks.cpp` |
| `2` | Stencil shadow | Build a shadow volume from model geometry and darken pixels inside that volume. | `code/rd-vanilla/tr_shadows.cpp`, `tr_mesh.cpp`, `tr_ghoul2.cpp`, `tr_backend.cpp` |
| `3` | Projection shadow | Flatten/project model geometry onto a shadow plane using a special projection shader. | `code/rd-vanilla/tr_shadows.cpp`, `tr_shader.cpp`, `tr_mesh.cpp`, `tr_ghoul2.cpp` |

### How the Game Chooses a Shadow Plane

Before the renderer can draw most character shadows, cgame finds the ground under the entity.

`CG_PlayerShadow()` in `code/cgame/cg_players.cpp` traces downward from a player or NPC. If the trace hits nearby ground, it records a `shadowPlane`, which is basically the Z height where the shadow should land.

In plain terms:

```text
"Shoot an invisible line downward from the character.
 If it hits the floor soon enough, put the shadow on that floor height."
```

If the character is too high above the ground, cloaked, a special no-shadow creature, or too far away according to `r_shadowRange` / `cg_shadowCullDistance`, cgame can skip the shadow.

When cgame submits the character's `refEntity_t`, it can set shadow-related render flags:

- `RF_SHADOW_PLANE`: this entity has a usable `shadowPlane`.
- `RF_NOSHADOW`: do not add renderer shadows for this entity.
- `RF_SHADOW_ONLY`: submit the entity only for shadowing, useful when the player body is hidden in first person but its shadow can still be drawn.
- `RF_DEPTHHACK`: excluded from stencil shadows to avoid bad depth behavior.

### Mode 1: Mark/Blob Shadows

For `cg_shadows 1`, the shadow is mostly a cgame mark, not a geometry shadow.

After the downward trace finds the floor, cgame calls `CG_ImpactMark()` with the `markShadow` shader. This creates a temporary dark mark on the floor. The mark fades with height: the farther the character is above the floor, the lighter the blob becomes.

This is cheap and stable, but it is not a true silhouette. It looks like a soft dark spot under the character.

### Mode 2: Stencil Shadows

For `cg_shadows 2`, the renderer uses the stencil buffer.

The rough flow is:

```text
cgame:
  -> trace to ground
  -> set RF_SHADOW_PLANE and shadowPlane on the entity

renderer front end:
  -> model submission sees r_shadows == 2
  -> if the surface is opaque and shadowable, add a drawSurf using tr.shadowShader

renderer backend:
  -> RB_ShadowTessEnd()
      -> build shadow volume geometry from model triangles
      -> mark covered pixels in the stencil buffer
  -> RB_ShadowFinish()
      -> draw a dark full-screen pass only where stencil != 0
```

The core code is in `code/rd-vanilla/tr_shadows.cpp`.

The stencil technique works by extruding model geometry away from a light direction, producing an invisible 3D shadow volume. The renderer does not directly draw black geometry first. It writes to the stencil buffer, which is a hidden per-pixel mask. Later, `RB_ShadowFinish()` darkens the pixels where the stencil mask says "this pixel is inside a shadow."

Important details:

- It requires a stencil buffer. The code returns early if `glConfig.stencilBits < 4`.
- It is delayed until after shadow volumes are accumulated so overlapping body-part shadows do not repeatedly darken the same area.
- The renderer uses a controlled projection based on the entity light direction and `shadowPlane`, partly to reduce artifacts like shadows showing through walls.
- `tr_init.cpp` checks whether `glStencilOpSeparate` is available so some stencil work can be done more efficiently.

Stencil shadows are more shape-accurate than blob shadows, but they are more fragile. The code comments note artifact risks from imperfect model edges and shadow volumes.

### Mode 3: Projection Shadows

For `cg_shadows 3`, the renderer projects model geometry onto the shadow plane using a special shader named `projectionShadow`.

Model submission adds an extra draw surface with `tr.projectionShadowShader`. Then `RB_ProjectionShadowDeform()` in `tr_shadows.cpp` flattens/projectively moves the model vertices toward the ground plane based on the entity's light direction.

In plain terms:

```text
"Take the character mesh, squash it onto the floor in the direction the light would cast it,
 and draw that squashed shape as a dark transparent shadow."
```

Projection shadows can look more like the model than a blob, and they do not rely on the stencil volume path. They are still limited because they project onto a simple plane, not complex stairs, walls, railings, or uneven geometry.

### Where Shadows Are Added to Models

Shadow draw surfaces are added during model submission:

- MD3 meshes: `code/rd-vanilla/tr_mesh.cpp`
- Ghoul2 skeletal models: `code/rd-vanilla/tr_ghoul2.cpp`

Both paths check similar conditions:

- Is `r_shadows` set to stencil or projection mode?
- Does the entity have `RF_SHADOW_PLANE`?
- Is the surface opaque?
- Is the entity allowed to cast shadows?
- Is the entity close enough for shadow rendering?

If the checks pass, the renderer adds an extra draw surface before or alongside the normal visible surface. That extra draw surface uses either the stencil shadow marker shader or the projection shadow shader.

### What Shadows Do Not Do

This renderer's shadows have important limits:

- Static world shadows mostly come from baked lightmaps, not runtime shadow casting.
- Dynamic lights generally do not cast full real-time shadows from every object.
- Blob and projection shadows rely on a simple floor plane found by cgame tracing.
- Stencil shadows depend on model geometry and stencil-buffer support.
- Fogged, transparent, depth-hacked, or explicitly no-shadow surfaces may not cast shadows.

The important takeaway: lighting and shadows are partly separate systems. Baked lightmaps provide most static darkness. Runtime shadows are mainly extra visual grounding for characters and models.

### Why CPU Ray-Traced Shadows Are Not a Drop-In Option

It is tempting to ask: if the CPU can already trace downward to find the floor, why not shoot more rays and make real shadows?

The short answer is that the current trace systems are useful for gameplay and shadow placement, but they are not a render-quality ray-tracing pipeline.

To make ray-traced shadows, the renderer would need to answer this question many times per frame:

```text
"For this exact visible point on this exact rendered surface,
 is there anything between it and the light?"
```

Doing that well requires data the current renderer does not maintain in a CPU-ray-friendly form:

- Exact visible surface points for many pixels or many mesh vertices.
- Fast ray intersections against render-detail BSP world geometry.
- Fast ray intersections against current-frame animated MD3 and Ghoul2 model triangles.
- A per-frame acceleration structure, such as a BVH, for all relevant static and dynamic render geometry.
- Correct handling for shader behavior such as alpha-tested grates, translucent materials, scrolling stages, and special effects.
- A way to feed the computed shadow result back into the existing OpenGL material pipeline.

The engine does have collision tracing, but collision geometry is not the same as render geometry. Collision traces are good for questions like:

```text
"Did this line hit a wall or floor?"
```

They are not designed for questions like:

```text
"Does this exact decorative mesh triangle or alpha-tested texture pixel block light?"
```

Animated Ghoul2 characters make this harder. Their rendered triangles are transformed by bones every frame. Accurate CPU ray-traced shadows would need those transformed triangles collected and organized for ray tests every frame. The current renderer submits and sorts model surfaces for OpenGL drawing; it does not build a CPU ray-tracing scene for them.

The cost also grows quickly. Even at 640x480, one ray per pixel is more than 300,000 rays for one light in one frame. At 60 FPS that is more than 18 million rays per second before multiple lights, soft-shadow samples, animated characters, transparency, or complex misses through the map. That is far outside what this renderer architecture was designed to do on the CPU.

Even if fewer rays were used, the result still needs somewhere to go. The backend expects sorted draw surfaces, shader stages, textures, lightmaps, fog passes, blend state, projection shadows, and stencil masks. It does not have a general CPU-generated shadow buffer stage that shades arbitrary pixels.

What is realistic in this architecture:

- Use CPU traces to find a floor or wall for fake shadows.
- Use traces to fade, hide, resize, or move blob/projection shadows.
- Use a small number of traces for special-case effects.
- Improve shadow placement for specific entities or surfaces.

What is not realistic as a small renderer change:

- Per-pixel CPU ray-traced shadows.
- Soft ray-traced shadows from many samples.
- Accurate ray shadows from animated Ghoul2 characters onto arbitrary world geometry.
- A general ray tracing pass integrated into the existing fixed-function OpenGL backend.

So the practical boundary is: CPU traces can make smarter fake shadows, but real CPU ray-traced shadows would require a new render data model and a new shadow integration path, not just a new function in `tr_shadows.cpp`.

## Entity and Model Rendering

After adding world surfaces, the renderer adds entities.

Entity rendering starts in `R_AddEntitySurfaces()` in `code/rd-vanilla/tr_main.cpp`.

The renderer checks each submitted `refEntity_t` and routes it based on type:

| Entity/model type | Renderer path |
| --- | --- |
| Sprites, beams, lines, electricity, saber glow | Drawn through generated/simple entity surfaces. |
| Brush models | Treated like small movable pieces of BSP geometry. |
| MD3 mesh models | Submitted through mesh rendering paths such as `R_AddMD3Surfaces`. |
| Ghoul2 skeletal models | Submitted through Ghoul2 paths such as `R_AddGhoulSurfaces`. |

Ghoul2 is especially important for Jedi Outcast characters. It handles skeletal model data: bones, animation frames, bolts, surfaces that can be hidden or shown, and model attachments. A practical example is a weapon attached to a hand bolt or a character surface toggled by game state.

## Shaders and Materials

In this renderer, "shader" usually does not mean a modern GLSL shader. It usually means a Quake 3-style material script.

A shader script can define:

- Which texture images to use.
- How stages blend together.
- Whether the surface is opaque, transparent, additive, emissive, fogged, or sorted specially.
- Texture scrolling, rotation, scaling, or turbulence.
- Lightmap behavior.
- Culling and depth behavior.

`code/rd-vanilla/tr_shader.cpp` parses these scripts and turns them into runtime `shader_t` data.

The renderer then uses shader sort order to decide when to draw a surface. For example:

- Portals/mirrors are considered early because they may need another view.
- Opaque world surfaces are drawn in efficient groups.
- Transparent effects are drawn later so blending works visually.

## Front End vs Back End

The existing generic renderer note says the renderer has a front end and a back end. For JK2 SP, that maps roughly like this:

| Part | Main files | Job |
| --- | --- | --- |
| Front end | `tr_scene.cpp`, `tr_main.cpp`, `tr_world.cpp`, model submission files | Collect the current scene, cull invisible work, generate draw surfaces, sort them, and queue render commands. |
| Back end | `tr_backend.cpp`, `tr_shade.cpp`, `tr_cmds.cpp` | Consume render commands, bind textures, set OpenGL state, batch surfaces, run shader stages, draw geometry, and present the frame. |

The front end avoids direct drawing where possible. It builds an ordered list. The back end consumes the list and performs the actual OpenGL work.

## Renderer Command Buffer

The renderer uses a command buffer between "deciding what to draw" and "drawing it."

`tr_cmds.cpp` contains the command submission side. A key example is `R_AddDrawSurfCmd(...)`, which queues a sorted list of draw surfaces for the backend.

The command buffer helps keep the renderer organized:

```text
Front end:
  "Here is the sorted list of surfaces for this view."

Back end:
  "I will now execute that list and issue OpenGL calls."
```

This design also allows multiple views in one frame. A frame can include the main 3D view, a mirror/portal view, a menu model preview, and then 2D UI drawing.

## OpenGL Initialization

Renderer startup happens mostly in `tr_init.cpp`.

The renderer:

- Receives window/OpenGL function hooks from the engine.
- Creates or uses the SDL/OpenGL window through imported `WIN_*` functions.
- Reads renderer cvars such as texture mode, dynamic lighting, fog behavior, gamma, shadows, and extension flags.
- Checks OpenGL extensions.
- Loads function pointers for supported extensions.
- Initializes images, shaders, models, fonts, and renderer globals.

For JK2 SP, notable renderer cvars and defaults include:

- `r_drawfog`: defaults to `1` in `JK2_MODE`.
- `sp_language`: used by JK2-specific language/font paths.
- `r_dynamiclight`, `r_fastsky`, `r_textureMode`, `r_gamma`, `r_shadows`, and many other classic renderer controls.

## JK2-Specific Renderer Differences

Because `rdjosp-vanilla` is built from shared SP renderer code, JK2-specific differences are mostly controlled by `#ifdef JK2_MODE`.

Important examples:

- Renderer name: JK2 SP defaults to `rdjosp-vanilla`; JKA SP defaults to `rdsp-vanilla`.
- Language/font handling: `tr_font.cpp` uses `sp_language` in JK2 mode for Asian language handling, while non-JK2 paths use different language checks.
- Save-game screenshots: JK2 mode exports `SaveJPGToBuffer` and `LoadJPGFromBuffer` through the renderer API. `code/server/sv_savegame.cpp` uses these to store and load save-game screenshots.
- Shader compatibility: `tr_shader.cpp` uses a different retail shader hash for `gfx/2d/wedge` under JK2 mode.
- Renderer cvars: at least some defaults differ, such as `r_drawfog`.
- Some Ghoul2/model/rendering behavior is gated by `JK2_MODE` to preserve JK2 behavior instead of JKA behavior.

These differences are small in architecture but important in behavior. The renderer is mostly the same machinery, but compiled with JK2 compatibility choices.

## Save-Game Screenshot Path

Jedi Outcast SP save games store preview screenshots. In JK2 mode, the renderer participates directly.

The flow is:

```text
Save game writes screenshot:
  -> server save-game code asks for/captures screenshot pixels
  -> renderer function re.SaveJPGToBuffer(...) compresses pixels
  -> save-game code writes JPG bytes into the save file

Save game reads screenshot:
  -> save-game code reads JPG bytes
  -> renderer function re.LoadJPGFromBuffer(...) decodes them
  -> decoded image can be shown in loading/save UI
```

This is a good example of why the renderer API includes more than "draw the world." It also owns image formats and screenshot helpers.

## Text, Fonts, and 2D Drawing

The renderer also draws 2D elements:

- HUD images.
- Menu graphics.
- Cinematic frames.
- Text and fonts.
- Loading screens and save-game screenshots.

2D drawing uses functions such as:

- `DrawStretchPic`
- `DrawRotatePic`
- `DrawStretchRaw`
- `RegisterFont`
- `Font_DrawString`

The important layman point: the renderer is not only responsible for 3D rooms and characters. It also draws flat screen-space elements on top of the 3D image.

## Mirrors, Portals, and Multiple Views

`RE_RenderScene()` can lead to more than one view being rendered.

Some surfaces act like portals or mirrors. When the draw-surface sorter sees a portal-like shader early in the sorted list, it may render another camera view first, then use that result in the main scene.

This is why the renderer tracks things like:

- `recursivePortalCount`
- portal render flags
- skybox portal flags
- scene counts and frame scene numbers

In plain terms: a single frame can contain a picture inside a picture. The renderer has to draw the inside picture before it can finish the outside picture.

## Visibility and Performance

The renderer avoids drawing everything. That is essential for performance.

It skips work using several layers:

- PVS: skip map leaves that the current area cannot see.
- Areamask: skip areas hidden by closed doors or area portals.
- Frustum culling: skip boxes outside the camera view.
- Entity type checks: skip first-person-only or third-person-only objects in the wrong view.
- Shader sorting: group surfaces to reduce expensive OpenGL state changes.
- Cvars: allow debug or performance controls such as `r_novis`, `r_nocull`, `r_drawentities`, or `r_drawworld`.

The renderer is therefore not just a drawer. It is also a filter and organizer.

## Mental Model

For a readable mental model, imagine the renderer as a theater crew:

| Theater role | Renderer equivalent |
| --- | --- |
| Stage manager | `refdef_t`: decides where the camera is and what scene is being staged. |
| Prop list | `refEntity_t` objects submitted by the game/client. |
| Building layout | BSP world data loaded from the map. |
| Lighting plan | Lightmaps, dynamic lights, fog, shader stages. |
| Costume/material notes | Shader scripts and skins. |
| Choreography | Ghoul2 bone animation and model attachments. |
| Running order | Sorted `drawSurf_t` list. |
| Camera crew | Backend OpenGL drawing code. |

The game says what should exist. The renderer decides what is visible, in what order it should be drawn, and how to draw it.

## Practical Navigation Guide

If you are changing the JK2 SP renderer, start here:

| Task | Start with |
| --- | --- |
| Renderer load/init problem | `code/client/cl_main.cpp`, `code/rd-vanilla/tr_init.cpp` |
| Renderer API mismatch or missing function | `code/rd-common/tr_public.h`, `code/rd-vanilla/tr_init.cpp` |
| Frame begin/end or screenshot command behavior | `code/rd-vanilla/tr_cmds.cpp` |
| Camera/view drawing behavior | `code/rd-vanilla/tr_scene.cpp`, `code/rd-vanilla/tr_main.cpp` |
| Map/world visibility | `code/rd-vanilla/tr_world.cpp`, `code/rd-vanilla/tr_bsp.cpp` |
| Lighting behavior | `code/rd-vanilla/tr_light.cpp`, `code/rd-vanilla/tr_bsp.cpp`, `code/rd-vanilla/tr_shader.cpp`, `code/rd-vanilla/tr_shade.cpp`, `code/cgame/cg_lights.cpp` |
| Shadow behavior | `code/cgame/cg_players.cpp`, `code/rd-vanilla/tr_shadows.cpp`, `code/rd-vanilla/tr_mesh.cpp`, `code/rd-vanilla/tr_ghoul2.cpp`, `code/rd-vanilla/tr_backend.cpp` |
| Model loading | `code/rd-vanilla/tr_model.cpp` |
| Ghoul2 character rendering | `code/rd-vanilla/tr_ghoul2.cpp`, `code/rd-vanilla/G2_*.cpp` |
| Shader/material behavior | `code/rd-vanilla/tr_shader.cpp`, `code/rd-vanilla/tr_shade.cpp` |
| Texture/image loading | `code/rd-vanilla/tr_image.cpp`, `code/rd-common/tr_image_*.cpp` |
| Font/language rendering | `code/rd-common/tr_font.cpp` |
| JK2 save-game screenshot handling | `code/server/sv_savegame.cpp`, `code/rd-common/tr_image_jpg.cpp` |

## Important Boundaries

- Do not assume multiplayer renderer behavior applies here. JK2 SP does not use `codemp/rd-rend2`.
- Do not assume `codeJK2/` owns the renderer. JK2 SP renderer behavior mostly comes from `code/rd-vanilla/` and `code/rd-common/` compiled with `JK2_MODE`.
- Be careful with public structs in `tr_public.h` and `tr_types.h`. They define the contract between the engine and renderer.
- Be careful with `JK2_MODE` conditionals. Many are compatibility decisions, not dead code.
- Rendering and gameplay are connected but separate. If something is wrong about what exists in the world, start in game/server/cgame code. If something exists but appears wrong, start in renderer code.
