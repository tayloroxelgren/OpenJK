# Software Renderer Roadmap

This document turns the renderer notes in `docs/` into a staged plan for adding a CPU software renderer to OpenJK. The goal is to make the work possible one section at a time without breaking the existing engine, game, cgame, UI, save-game, or asset-loading contracts.

The recommended first target is the single-player vanilla renderer path under `code/rd-vanilla/`, with a new renderer module target beside the OpenGL renderer. Multiplayer can follow after the core renderer API, asset registration, and frame presentation path are proven.

## Referenced Documents

- [architecture-overview.md](architecture-overview.md)
- [renderer-architecture.md](renderer-architecture.md)
- [jedi-outcast-sp-renderer-architecture.md](jedi-outcast-sp-renderer-architecture.md)
- [game-loading-system-architecture.md](game-loading-system-architecture.md)
- [save games.md](save%20games.md)
- [language strings.md](language%20strings.md)
- [libraries.md](libraries.md)
- [game-audio-system-architecture.md](game-audio-system-architecture.md)
- [sound-code-declutter.md](sound-code-declutter.md)

## Main Constraints From The Existing Architecture

1. Keep the renderer module boundary stable.
   The engine loads renderers dynamically through `GetRefAPI(...)`, `refimport_t`, and `refexport_t`. A software renderer should fit behind that boundary instead of changing cgame, UI, or game code first.

2. Keep the front end and back end split.
   The current renderer collects scene data in the front end, sorts draw surfaces, then lets the back end draw them. A software renderer should preserve that shape. The difference is that the back end writes pixels into CPU buffers instead of issuing OpenGL draw calls.

3. Respect the loading order.
   Renderer startup and world loading are separate. `re.BeginRegistration` initializes renderer systems, while cgame later calls `cgi_R_LoadWorldMap(...)` during graphics registration. Savegames also load the map normally before restoring serialized state.

4. Do not use collision geometry as render geometry.
   `CM_LoadMap` loads the BSP for gameplay traces and collision. `RE_LoadWorldMap` loads visual BSP data for rendering. A software renderer needs visual surfaces, shaders, lightmaps, fog, and model data, not just collision hulls.

5. Preserve asset and configstring behavior.
   Server/game code publishes media references through configstrings. Cgame registers graphics, sounds, models, shaders, skins, effects, and BSP instances during level load. The software renderer should consume the same registrations.

6. Treat 2D as part of the renderer.
   HUD, menus, loading screens, cinematics, text, save previews, and raw images all go through renderer APIs. A renderer that only draws 3D world triangles is not enough.

7. Keep JK2/JKA compatibility gates intentional.
   JK2 SP behavior comes from the shared SP renderer compiled with `JK2_MODE`. Any new renderer target needs a clear answer for JK2 mode, JKA mode, or both.

8. Prefer incremental replacement.
   The audio declutter notes are not renderer code, but they describe a useful strategy: keep the public API stable, isolate the messy implementation, then replace internals in controlled layers.

## Suggested Target Layout

Add a new renderer implementation rather than rewriting `rd-vanilla` in place.

Suggested layout:

```text
code/rd-software/
  CMakeLists.txt
  sr_init.cpp
  sr_cmds.cpp
  sr_framebuffer.cpp
  sr_image.cpp
  sr_shader.cpp
  sr_bsp.cpp
  sr_scene.cpp
  sr_world.cpp
  sr_raster.cpp
  sr_model.cpp
  sr_mesh.cpp
  sr_ghoul2.cpp
  sr_font.cpp
  sr_local.h
```

Shared code should still come from `code/rd-common/` where practical:

- public renderer API: `code/rd-common/tr_public.h`
- public render types: `code/rd-common/tr_types.h`
- image decoders: `code/rd-common/tr_image_*.cpp`
- font helpers where reusable: `code/rd-common/tr_font.cpp`

The first version can also copy small pieces from `code/rd-vanilla/` when that is safer than trying to over-generalize old OpenGL-oriented code immediately. Once the software renderer works, shared parsing and asset code can be factored more cleanly.

## Section 1: Renderer Module Skeleton

### Goal

Create a loadable renderer module that the engine can initialize, call, and shut down without drawing anything meaningful yet.

### How It Would Be Done

1. Add a new CMake option and target, for example:

   ```text
   BuildSPRdSoftware
   rdsp-software_<arch>
   ```

   For JK2 SP, add a matching target later:

   ```text
   BuildJK2SPRdSoftware
   rdjosp-software_<arch>
   ```

2. Export `GetRefAPI(...)` from the new renderer target.

3. Fill a `refexport_t` table with stubbed or minimal implementations for the renderer API used by the SP client.

4. Store the incoming `refimport_t` so the renderer can use engine services such as filesystem, cvars, error reporting, memory allocation, timing, and window hooks.

5. Add enough cvars and logging to identify the renderer at startup.

6. Allow the engine to select it through `cl_renderer`, the same way `rdsp-vanilla` or `rdjosp-vanilla` is selected.

### Main Files

- `CMakeLists.txt`
- `code/rd-software/CMakeLists.txt`
- `code/rd-software/sr_init.cpp`
- `code/rd-common/tr_public.h`
- `code/client/cl_main.cpp`

### Complete When

- The engine can load the software renderer module.
- `GetRefAPI(...)` version checks work.
- `re.BeginRegistration`, `re.EndRegistration`, `re.BeginFrame`, `re.EndFrame`, and `re.Shutdown` can be called without crashing.
- The renderer can print a clear startup line such as `Software renderer initialized`.

## Section 2: CPU Framebuffer And Present Path

### Goal

Create the core software rendering target: a CPU-owned color buffer and depth buffer.

### How It Would Be Done

1. Add a framebuffer structure:

   ```cpp
   struct srFramebuffer_t {
       int width;
       int height;
       uint32_t *color;
       float *depth;
   };
   ```

2. Allocate or resize the buffers during frame begin or video mode changes.

3. Clear the color buffer and depth buffer each frame.

4. Add a single present path that displays the CPU buffer.

   The first version can use a minimal upload-to-window path even if that uses a tiny amount of OpenGL or SDL texture presentation internally. That presentation layer must stay isolated: all triangle rasterization, depth testing, blending, lightmaps, and shading should happen on the CPU.

5. Later, replace the bootstrap present path with an SDL software texture or platform path if the engine window code allows it cleanly.

### Main Files

- `code/rd-software/sr_framebuffer.cpp`
- `code/rd-software/sr_cmds.cpp`
- `shared/sdl/`
- `code/rd-vanilla/tr_cmds.cpp`
- `code/rd-vanilla/tr_init.cpp`

### Complete When

- The screen shows a stable clear color or test pattern.
- Resizing or changing mode reallocates buffers safely.
- No world, model, or UI drawing is required yet.

## Section 3: 2D Draw Calls

### Goal

Make menus, loading screens, HUD images, and simple renderer tests visible before implementing the full 3D world.

### How It Would Be Done

1. Implement these `refexport_t` functions first:

   ```text
   DrawStretchPic
   DrawRotatePic
   DrawStretchRaw
   RegisterShader
   RegisterShaderNoMip
   RegisterFont
   Font_DrawString
   ```

2. Treat 2D as textured rectangle rasterization into the CPU color buffer.

3. Start with nearest-neighbor sampling and simple alpha blending.

4. Add clipping against the framebuffer bounds.

5. Support raw image upload for cinematics and loading imagery through CPU-side pixel storage.

### Main Files

- `code/rd-software/sr_cmds.cpp`
- `code/rd-software/sr_image.cpp`
- `code/rd-software/sr_font.cpp`
- `code/rd-common/tr_font.cpp`
- `code/rd-vanilla/tr_cmds.cpp`

### Complete When

- Loading screen images can draw.
- UI/HUD rectangles can draw.
- Text can draw well enough to navigate menus and see errors.

## Section 4: Image Loading Without GL Upload

### Goal

Reuse existing image decoders while keeping texture data available in CPU memory.

### How It Would Be Done

1. Split image handling into two ideas:

   ```text
   decode image file -> CPU pixels
   upload/register texture -> renderer-owned texture handle
   ```

2. Reuse decoders from `code/rd-common/tr_image_*.cpp`.

3. Store software texture data as RGBA8 initially:

   ```cpp
   struct srImage_t {
       char name[MAX_QPATH];
       int width;
       int height;
       uint32_t *mipPixels[MAX_MIP_LEVELS];
       int mipWidth[MAX_MIP_LEVELS];
       int mipHeight[MAX_MIP_LEVELS];
       bool clampToEdge;
       bool noMip;
   };
   ```

4. Implement a small texture lookup table compatible with renderer shader/model registration.

5. Generate mip levels on the CPU after the base image works.

### Main Files

- `code/rd-software/sr_image.cpp`
- `code/rd-common/tr_image_jpg.cpp`
- `code/rd-common/tr_image_png.cpp`
- `code/rd-common/tr_image_tga.cpp`
- `code/rd-vanilla/tr_image.cpp`

### Complete When

- `RegisterShader` and `RegisterShaderNoMip` can resolve image paths.
- Missing image behavior is visible and stable.
- CPU-side textures are available for 2D drawing.

## Section 5: Font And Language Rendering

### Goal

Support menu, HUD, loading, and save-game text with the existing language behavior.

### How It Would Be Done

1. Reuse or adapt `code/rd-common/tr_font.cpp`.

2. Keep the renderer-facing font API compatible with existing UI/cgame calls.

3. Respect JK2-specific language behavior where `JK2_MODE` uses `sp_language`.

4. Make glyph rendering write textured quads into the software framebuffer.

5. Keep language string file parsing outside the renderer; the renderer only draws the already-resolved strings.

### Main Files

- `code/rd-software/sr_font.cpp`
- `code/rd-common/tr_font.cpp`
- `code/rd-common/tr_types.h`
- `code/qcommon/stringed_ingame.cpp`
- `code/qcommon/stringed_interface.cpp`

### Complete When

- Menus and loading strings render legibly.
- Font registration works across a normal map load.
- JK2/JKA language-specific font behavior is not accidentally removed.

## Section 6: Screenshot And Save Preview Support

### Goal

Keep save-game preview screenshots and screenshot commands working.

### How It Would Be Done

1. Make screenshot capture read from the CPU color framebuffer.

2. Reuse `rd-common` JPG helpers for save-game preview encode/decode.

3. Implement JK2-mode exports such as:

   ```text
   SaveJPGToBuffer
   LoadJPGFromBuffer
   ```

4. Keep row order, color channel order, and dimensions compatible with current save-game code.

### Main Files

- `code/rd-software/sr_cmds.cpp`
- `code/rd-common/tr_image_jpg.cpp`
- `code/server/sv_savegame.cpp`
- `code/rd-vanilla/tr_cmds.cpp`

### Complete When

- Screenshot command creates a readable image.
- JK2 save-game preview images can be encoded and decoded.
- Loading an old save does not break because the renderer helper is missing.

## Section 7: Visual BSP Loading

### Goal

Load renderable world geometry into software-renderer-owned structures.

### How It Would Be Done

1. Start from the visual BSP loading behavior in `code/rd-vanilla/tr_bsp.cpp`.

2. Preserve the separation from collision loading in `code/qcommon/cm_load.cpp`.

3. Load these visual BSP pieces:

   ```text
   shaders
   lightmaps
   planes
   fog volumes
   draw vertices
   draw indexes
   draw surfaces
   marksurfaces
   nodes
   leafs
   submodels
   visibility
   worldspawn render keys
   light grid
   light grid array
   ```

4. Convert loaded surfaces into CPU-friendly surface records:

   ```cpp
   struct srWorldSurface_t {
       int firstIndex;
       int numIndexes;
       int firstVertex;
       int numVertices;
       int shaderIndex;
       int fogIndex;
       int lightmapIndex;
       bounds_t bounds;
   };
   ```

5. Keep the renderer world lifetime tied to registration and map load, not global engine lifetime.

### Main Files

- `code/rd-software/sr_bsp.cpp`
- `code/rd-software/sr_world.cpp`
- `code/rd-vanilla/tr_bsp.cpp`
- `code/rd-vanilla/tr_world.cpp`
- `code/qcommon/cm_load.cpp`

### Complete When

- `cgi_R_LoadWorldMap(cgs.mapname)` succeeds.
- BSP metadata, surfaces, lightmaps, fog, and visibility data are loaded.
- No triangles need to draw yet, but the world data must be inspectable through debug logging.

## Section 8: Camera, Projection, Clipping, And Depth

### Goal

Transform world-space triangles into framebuffer pixels.

### How It Would Be Done

1. Read camera data from `refdef_t`:

   ```text
   x, y, width, height
   fov_x, fov_y
   vieworg
   viewaxis
   time
   areamask
   rdflags
   ```

2. Build view and projection transforms equivalent to the vanilla renderer.

3. Clip triangles against the near plane first. Add full frustum clipping later if needed.

4. Convert normalized projected coordinates into viewport pixels.

5. Add depth testing:

   ```text
   pass if new depth is closer
   write depth for opaque surfaces
   skip or conditionally write depth for translucent surfaces
   ```

6. Use fixed-point or float edge functions for the first triangle rasterizer. Optimize later.

### Main Files

- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_scene.cpp`
- `code/rd-vanilla/tr_main.cpp`
- `code/rd-common/tr_types.h`

### Complete When

- A hard-coded triangle can be drawn from a `refdef_t` camera.
- Depth testing works between overlapping triangles.
- Viewport rectangles are honored.

## Section 9: Flat World Rasterization

### Goal

Draw visible BSP world triangles in simple solid colors.

### How It Would Be Done

1. During `RE_RenderScene`, collect world surfaces the same way the front end already does conceptually:

   ```text
   R_AddWorldSurfaces
   R_MarkLeaves
   R_RecursiveWorldNode
   ```

2. For the first version, draw every loaded world surface without PVS optimization if that is faster to bring up.

3. Convert each surface's indexed triangles into rasterizer input.

4. Assign debug colors by shader index, lightmap index, or surface type.

5. Draw opaque triangles with depth testing.

### Main Files

- `code/rd-software/sr_world.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_world.cpp`
- `code/rd-vanilla/tr_main.cpp`

### Complete When

- A loaded map renders as solid-color geometry.
- Moving the camera changes the view correctly.
- Depth ordering between world surfaces is stable.

## Section 10: Texture Sampling

### Goal

Draw textured world triangles.

### How It Would Be Done

1. Add texture coordinates to the rasterizer vertex format:

   ```cpp
   struct srRasterVertex_t {
       float x, y, z, w;
       float s, t;
       uint32_t color;
   };
   ```

2. Implement affine texture mapping first.

3. Add perspective-correct interpolation:

   ```text
   interpolate s / w
   interpolate t / w
   interpolate 1 / w
   recover s and t per pixel
   ```

4. Implement nearest sampling first.

5. Add bilinear filtering after correctness is stable.

6. Add mip selection after surfaces are textured and stable.

### Main Files

- `code/rd-software/sr_raster.cpp`
- `code/rd-software/sr_image.cpp`
- `code/rd-vanilla/tr_shade.cpp`
- `code/rd-vanilla/tr_shade_calc.cpp`

### Complete When

- World surfaces render with their base textures.
- Texture coordinates do not swim badly when the camera moves.
- Missing textures are obvious but not fatal.

## Section 11: Lightmap Combination

### Goal

Make static world lighting resemble the original renderer.

### How It Would Be Done

1. Load BSP lightmaps as CPU textures.

2. Preserve overbright and color-shift behavior from the vanilla renderer.

3. For each world surface, sample both:

   ```text
   base texture
   lightmap texture
   ```

4. Combine them in the rasterizer:

   ```text
   final.rgb = base.rgb * lightmap.rgb * scale
   final.a = base.a
   ```

5. Handle surfaces without lightmaps through fallback lighting.

6. Add light style modulation later.

### Main Files

- `code/rd-software/sr_bsp.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_bsp.cpp`
- `code/rd-vanilla/tr_shader.cpp`
- `code/rd-vanilla/tr_shade.cpp`

### Complete When

- Static rooms show baked brightness and color from BSP lightmaps.
- Unlit or special surfaces still draw with a clear fallback.
- Lightmap texture coordinates are correct.

## Section 12: Shader Script Subset

### Goal

Support the material behavior needed by common maps before attempting the full shader language.

### How It Would Be Done

1. Parse or reuse parsed Quake 3-style shader data.

2. Implement a narrow first subset:

   ```text
   base texture stage
   $lightmap stage
   blendFunc
   alphaFunc
   rgbGen identity
   rgbGen vertex
   rgbGen lightingDiffuse
   tcMod scroll
   tcMod scale
   tcMod rotate
   cull
   depthWrite
   depthFunc
   sort
   fogParms
   ```

3. Treat unsupported shader features as warnings with visible fallbacks.

4. Preserve shader sort order because it affects portals, opaque drawing, fog, and transparency.

5. Add a CPU shader-stage interpreter:

   ```text
   input surface + shader stages + interpolated attributes
   output blended pixel
   ```

### Main Files

- `code/rd-software/sr_shader.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_shader.cpp`
- `code/rd-vanilla/tr_shade.cpp`

### Complete When

- Common opaque, lightmapped, additive, alpha-tested, and blended surfaces draw acceptably.
- Unsupported shaders are visible in logs.
- Shader sort order is respected.

## Section 13: BSP Visibility And Culling

### Goal

Avoid drawing the whole map every frame.

### How It Would Be Done

1. Implement PVS leaf marking from loaded BSP visibility data.

2. Apply the `refdef_t` areamask so closed portals and doors can hide areas.

3. Implement frustum culling for BSP nodes and surface bounds.

4. Add cvar overrides similar to the vanilla renderer:

   ```text
   r_novis
   r_nocull
   r_drawworld
   ```

5. Keep culling in the front end. The rasterizer should receive only surfaces that were chosen for drawing.

### Main Files

- `code/rd-software/sr_world.cpp`
- `code/rd-software/sr_scene.cpp`
- `code/rd-vanilla/tr_world.cpp`
- `code/rd-vanilla/tr_main.cpp`

### Complete When

- Map rendering cost drops when looking away from large areas.
- `r_novis` and `r_nocull` style debug behavior can confirm the culling path.
- Closed area portals affect visibility.

## Section 14: Draw Surface Sorting And Transparency

### Goal

Draw surfaces in an order compatible with opaque rendering, fog, portals, and blended effects.

### How It Would Be Done

1. Keep or mirror the existing `drawSurf_t` sorting model.

2. Build sort keys from:

   ```text
   shader sort
   shader index
   entity index
   fog index
   dynamic-light bits
   depth bucket for blended surfaces
   ```

3. Draw opaque surfaces first with depth writes enabled.

4. Draw alpha-tested surfaces with depth writes as appropriate.

5. Draw translucent surfaces later with blending.

6. Use approximate back-to-front sorting for translucent surfaces where shader sort alone is not enough.

### Main Files

- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_main.cpp`
- `code/rd-vanilla/tr_backend.cpp`

### Complete When

- Glass, additive effects, grates, and translucent surfaces are visually plausible.
- Opaque surfaces still depth-test correctly.
- Surface ordering bugs can be debugged from sort keys.

## Section 15: Brush Models And Inline BSP Models

### Goal

Render doors, movers, platforms, and inline map models.

### How It Would Be Done

1. Use existing model registration names such as:

   ```text
   *0
   *1
   *<subBspIndex>-<subModelIndex>
   ```

2. Load inline BSP model bounds and surfaces from the visual BSP data.

3. Apply entity transforms before rasterization.

4. Reuse world-surface drawing for brush model surfaces.

5. Respect cgame/server-provided model handles from configstrings.

### Main Files

- `code/rd-software/sr_model.cpp`
- `code/rd-software/sr_world.cpp`
- `code/rd-vanilla/tr_model.cpp`
- `code/rd-vanilla/tr_bsp.cpp`
- `code/cgame/cg_main.cpp`

### Complete When

- Doors, platforms, and inline map geometry draw in the correct place.
- Sub-BSP instances can register without crashing.
- Brush model bounds and culling work.

## Section 16: MD3 Mesh Models

### Goal

Render static and animated MD3-style mesh models.

### How It Would Be Done

1. Reuse model registration behavior from `tr_model.cpp`.

2. Load MD3 frames, tags, surfaces, triangles, texture coordinates, and shader references.

3. Interpolate vertex positions between frames when needed.

4. Transform model vertices by the entity origin and axis.

5. Submit model triangles through the same software rasterizer used for world surfaces.

6. Add entity render flags such as `RF_NOSHADOW`, `RF_DEPTHHACK`, and `RF_THIRD_PERSON` as needed.

### Main Files

- `code/rd-software/sr_model.cpp`
- `code/rd-software/sr_mesh.cpp`
- `code/rd-vanilla/tr_model.cpp`
- `code/rd-vanilla/tr_mesh.cpp`
- `code/rd-common/tr_types.h`

### Complete When

- MD3 props and simple animated models render.
- Frame interpolation is stable.
- Model shaders resolve through the software shader system.

## Section 17: Light Grid Entity Lighting

### Goal

Light moving models using baked BSP light-grid data.

### How It Would Be Done

1. Load the BSP light grid during world load.

2. For each lit entity, sample the eight surrounding grid points.

3. Blend ambient light, directed light, and light direction by trilinear interpolation.

4. Apply active light styles.

5. Feed the result into model shading, initially as per-vertex color.

6. Support `RF_LIGHTING_ORIGIN` for entities that provide a custom lighting point.

### Main Files

- `code/rd-software/sr_bsp.cpp`
- `code/rd-software/sr_model.cpp`
- `code/rd-vanilla/tr_light.cpp`
- `code/rd-vanilla/tr_bsp.cpp`

### Complete When

- Models brighten and darken as they move through the map.
- Entity lighting roughly matches vanilla behavior.
- Debug output can show sampled ambient and directed colors.

## Section 18: Ghoul2 Skeletal Models

### Goal

Render Jedi Outcast and Jedi Academy character models.

### How It Would Be Done

1. Reuse Ghoul2 loading, animation, bone, bolt, and surface logic as much as possible.

2. Identify the point where vanilla Ghoul2 rendering has final transformed triangles or can produce them.

3. Convert transformed Ghoul2 surfaces into software rasterizer input.

4. Preserve surface hide/show behavior, attachments, bolts, and skin overrides.

5. Apply light-grid and dynamic-light model lighting.

6. Add render flags and sorting behavior used by player/NPC rendering.

### Main Files

- `code/rd-software/sr_ghoul2.cpp`
- `code/rd-vanilla/tr_ghoul2.cpp`
- `code/rd-vanilla/G2_*.cpp`
- `code/ghoul2/`
- `code/cgame/cg_players.cpp`

### Complete When

- Player/NPC characters draw with animation.
- Weapon attachments and bolt-based attachments appear in the right place.
- Surface toggles and skins work.

## Section 19: Dynamic Lights

### Goal

Support temporary lights from weapons, explosions, effects, pickups, and scripts.

### How It Would Be Done

1. Store lights submitted through `re.AddLightToScene(...)`.

2. For models, add nearby dynamic lights to light-grid lighting.

3. For world surfaces, start with per-vertex dynamic light contribution.

4. Add optional per-pixel dynamic light only after per-vertex lighting is stable.

5. Respect `r_dynamiclight`.

6. Keep the maximum light count compatible with `MAX_DLIGHTS`.

### Main Files

- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_scene.cpp`
- `code/rd-vanilla/tr_light.cpp`

### Complete When

- Explosions, weapons, and scripted lights visibly affect nearby models or surfaces.
- `r_dynamiclight 0` disables the feature.
- Dynamic lights do not break lightmapped world rendering.

## Section 20: Polys, Particles, Beams, Lines, And Sabers

### Goal

Render the high-value effect primitives submitted directly by cgame.

### How It Would Be Done

1. Implement `AddPolyToScene` and custom polygon submission.

2. Implement entity types used for:

   ```text
   sprites
   beams
   lines
   electricity
   saber glow
   portal markers
   ```

3. Route generated geometry through the same rasterizer and shader subset.

4. Pay special attention to additive blending and sort order.

5. Keep these effects independent from gameplay. Cgame decides they exist; the renderer only draws them.

### Main Files

- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_scene.cpp`
- `code/rd-vanilla/tr_main.cpp`
- `code/cgame/cg_event.cpp`
- `code/cgame/cg_ents.cpp`

### Complete When

- Particles, weapon effects, saber glows, beams, and electricity are visible.
- Additive effects blend over the world.
- Effect-heavy scenes remain stable.

## Section 21: Fog

### Goal

Apply BSP fog volumes and shader fog behavior.

### How It Would Be Done

1. Load fog definitions from the BSP and shader `fogParms`.

2. Associate world surfaces, models, and polygons with fog volumes.

3. Compute fog amount per vertex first.

4. Add per-pixel fog interpolation in the rasterizer.

5. Blend final pixels toward fog color based on distance or fog volume rules.

### Main Files

- `code/rd-software/sr_bsp.cpp`
- `code/rd-software/sr_shader.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_bsp.cpp`
- `code/rd-vanilla/tr_shader.cpp`
- `code/rd-vanilla/tr_shade_calc.cpp`

### Complete When

- Fogged rooms and outdoor fog volumes look plausible.
- `r_drawfog` behavior can be honored.
- Fogged transparent surfaces do not obviously break sorting.

## Section 22: Marks And Decals

### Goal

Support wall marks, scorch marks, impact marks, and blob shadows.

### How It Would Be Done

1. Implement mark polygon drawing from cgame submissions.

2. Clip marks against target surfaces where needed.

3. Draw marks as blended or alpha-tested polygons with depth bias.

4. Preserve cgame-created blob shadow behavior for `cg_shadows 1`.

5. Keep mark lifetime management in cgame unless the current renderer API already owns a piece of it.

### Main Files

- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/cgame/cg_marks.cpp`
- `code/cgame/cg_players.cpp`
- `code/rd-vanilla/tr_shade.cpp`

### Complete When

- Weapon impacts and character blob shadows draw.
- Marks do not z-fight excessively.
- Marks fade or disappear according to existing cgame behavior.

## Section 23: Projection And Stencil-Style Shadows

### Goal

Add runtime model shadows after basic models and marks work.

### How It Would Be Done

1. Keep `cg_shadows 1` blob marks as the first supported mode.

2. Add projection shadows before stencil shadows:

   ```text
   find shadow plane from cgame
   project model vertices onto plane
   draw projected geometry with transparent shadow shader
   ```

3. Treat stencil shadows as optional or late because a CPU software renderer has no hardware stencil buffer.

4. If stencil-like shadows are desired, add a CPU stencil mask buffer:

   ```cpp
   uint8_t *stencil;
   ```

   Then emulate the specific shadow volume behavior needed by the game.

5. Do not attempt general CPU ray-traced shadows as part of this section. The renderer docs explain why that requires a different scene data model and acceleration structure.

### Main Files

- `code/rd-software/sr_raster.cpp`
- `code/rd-software/sr_model.cpp`
- `code/rd-vanilla/tr_shadows.cpp`
- `code/rd-vanilla/tr_mesh.cpp`
- `code/rd-vanilla/tr_ghoul2.cpp`
- `code/cgame/cg_players.cpp`

### Complete When

- Blob shadows work.
- Projection shadows work for basic models on a shadow plane.
- Shadow modes fail gracefully when unsupported.

## Section 24: Portals, Mirrors, And Multiple Views

### Goal

Support scenes that require rendering one view inside another view.

### How It Would Be Done

1. Preserve the existing recursive-view concept.

2. Detect portal or mirror surfaces from shader sort/material data.

3. Render the portal view into a separate CPU framebuffer or temporary texture.

4. Use that result as an input texture when drawing the portal surface in the main view.

5. Enforce recursion limits equivalent to the vanilla renderer.

6. Add skybox portal behavior separately after mirror-style portals work.

### Main Files

- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_world.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_main.cpp`
- `code/rd-vanilla/tr_backend.cpp`

### Complete When

- Mirror or portal test maps render a recursive view.
- Recursion limit prevents runaway rendering.
- Main-scene rendering still works when no portals are visible.

## Section 25: Renderer Cvars And Debug Views

### Goal

Make the software renderer debuggable and comparable to the vanilla renderer.

### How It Would Be Done

1. Register software-renderer-specific cvars:

   ```text
   r_softWireframe
   r_softMip
   r_softBilinear
   r_softOverdraw
   r_softStats
   r_softDrawSurfs
   ```

2. Support existing useful renderer cvars where possible:

   ```text
   r_drawworld
   r_drawentities
   r_novis
   r_nocull
   r_dynamiclight
   r_fastsky
   r_drawfog
   r_shadows
   ```

3. Add debug visualizations:

   ```text
   solid shader color
   lightmap only
   texture only
   depth
   overdraw
   fog factor
   PVS leaves
   ```

4. Print per-frame counters when requested:

   ```text
   surfaces submitted
   triangles rasterized
   pixels shaded
   pixels depth-rejected
   draw time
   present time
   ```

### Main Files

- `code/rd-software/sr_init.cpp`
- `code/rd-software/sr_scene.cpp`
- `code/rd-software/sr_raster.cpp`
- `code/rd-vanilla/tr_init.cpp`

### Complete When

- Renderer issues can be isolated without attaching a debugger.
- Performance counters exist.
- Common vanilla debug cvars have reasonable software equivalents.

## Section 26: Performance Pass

### Goal

Make the software renderer usable after correctness is established.

### How It Would Be Done

1. Profile before optimizing.

2. Improve memory layout for hot rasterizer data:

   ```text
   contiguous vertices
   contiguous indexes
   cache-friendly texture mips
   compact shader-stage state
   ```

3. Add tiled rendering or scanline batching.

4. Add fixed-point inner loops where they help.

5. Add SIMD for common paths:

   ```text
   depth test
   texture sampling
   lightmap combine
   alpha blend
   clear buffers
   ```

6. Add mip selection and optional lower-resolution rendering.

7. Avoid per-pixel branches in common opaque lightmapped paths.

8. Keep a simple scalar path for portability and debugging.

### Main Files

- `code/rd-software/sr_raster.cpp`
- `code/rd-software/sr_framebuffer.cpp`
- `code/rd-software/sr_shader.cpp`
- `shared/qcommon/`

### Complete When

- A small map is interactive at low resolution.
- Hot paths are measurable.
- Debug scalar rendering still works.

## Section 27: Multiplayer Extension

### Goal

Bring the software renderer to the multiplayer renderer path after SP is stable.

### How It Would Be Done

1. Compare SP and MP renderer APIs and public types.

2. Add an MP target such as:

   ```text
   rd-software_<arch>
   ```

3. Reuse the shared software backend where possible.

4. Add compatibility for MP cgame/UI registration behavior.

5. Decide whether MP-specific renderer differences require separate compile flags or runtime feature flags.

6. Avoid starting with MP `rend2`; begin from the vanilla MP renderer contract.

### Main Files

- `codemp/rd-vanilla/`
- `codemp/rd-common/`
- `codemp/client/`
- `code/rd-software/`
- `CMakeLists.txt`

### Complete When

- MP client can load the software renderer.
- MP UI and cgame can render basic 2D.
- A simple MP map can render with the same core software backend.

## Practical Implementation Order

The safest order is:

```text
1. Renderer module skeleton
2. CPU framebuffer and present path
3. 2D draw calls
4. Image loading without GL upload
5. Font and language rendering
6. Screenshot and save preview support
7. Visual BSP loading
8. Camera, projection, clipping, and depth
9. Flat world rasterization
10. Texture sampling
11. Lightmap combination
12. Shader script subset
13. BSP visibility and culling
14. Draw surface sorting and transparency
15. Brush models and inline BSP models
16. MD3 mesh models
17. Light grid entity lighting
18. Ghoul2 skeletal models
19. Dynamic lights
20. Polys, particles, beams, lines, and sabers
21. Fog
22. Marks and decals
23. Projection and stencil-style shadows
24. Portals, mirrors, and multiple views
25. Renderer cvars and debug views
26. Performance pass
27. Multiplayer extension
```

This order gets visible output early, keeps the engine/module ABI stable, and delays the hardest features until the renderer already has assets, world geometry, model geometry, materials, sorting, and debug tools.

## Suggested First Milestone

The first milestone should be deliberately small:

```text
Build and load rdsp-software.
Open a frame.
Clear a CPU color buffer.
Present a test pattern.
Draw 2D textured rectangles.
Draw text.
Take a screenshot from the CPU buffer.
```

That milestone proves the module boundary, window/present path, image loading, UI drawing, text drawing, and screenshots before any BSP or model work begins.

## Features To Avoid In Early Sections

Do not start with these:

- CPU ray-traced shadows.
- Full Ghoul2 character rendering.
- Full shader-script compatibility.
- Portals and recursive views.
- Stencil shadow emulation.
- Multiplayer `rend2` compatibility.
- Large renderer-wide refactors.

Those are real features, but they depend on too much infrastructure. They belong after the renderer can already load assets, draw 2D, draw the world, draw textured/lightmapped surfaces, and debug itself.

## Definition Of Done For Each Section

Each section should finish with:

1. A renderer cvar or test command that can exercise the feature.
2. A simple map or UI path that visibly confirms the feature.
3. A debug log or counter that confirms the expected path ran.
4. A graceful fallback for unsupported data.
5. No changes required in game rules, save-game state, or cgame scene submission unless that section explicitly calls for it.

