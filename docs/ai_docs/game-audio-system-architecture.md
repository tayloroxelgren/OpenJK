# Game Audio System Architecture

This document explains how OpenJK plays game sound effects: weapon shots, footsteps, saber hums, UI beeps, NPC voice lines, movers, explosions, ambient loops, and similar short sounds.

The short version is:

1. Game code asks for a sound by name or by a precached sound index.
2. Cgame turns gameplay events and entity state into client sound calls.
3. The client sound system finds or loads the sound data.
4. The sound is assigned to a limited set of playback channels.
5. Each frame, active sounds are positioned around the listener, mixed into a stereo buffer, and sent to SDL or the legacy OpenAL path.

The game is not using a modern middleware-style audio engine. There are no high-level buses, authored sound graphs, occlusion volumes, DSP effect chains, or runtime mixing nodes. It is an older Quake 3/Raven style sound system built around simple handles, entity numbers, channels, left/right volume, and a software mixer.

## Main Source Areas

| Area | Important files | Role |
| --- | --- | --- |
| Public sound API | `code/client/snd_public.h` | Functions used by the client/cgame layer, such as `S_StartSound`, `S_RegisterSound`, `S_Update`, and `S_AddLoopingSound`. |
| Main sound controller | `code/client/snd_dma.cpp` | Initializes sound, tracks channels, starts sounds, spatializes sounds, manages loops, updates the mixer each frame, and handles legacy OpenAL support. |
| Software mixer | `code/client/snd_mix.cpp` | Mixes active sound channels into the final output buffer. |
| Sound loading/cache | `code/client/snd_mem.cpp` | Loads WAV/MP3 files, resamples them, caches decoded data, and handles missing sounds/language fallback. |
| Private sound structures | `code/client/snd_local.h` | Defines `sfx_t`, `channel_t`, `dma_t`, limits, and mixer globals. |
| SDL output | `shared/sdl/sdl_sound.cpp` | Opens the OS audio device and copies mixed samples to SDL's audio callback. |
| Cgame syscall bridge | `code/cgame/cg_syscalls.cpp`, `code/client/cl_cgame.cpp` | Lets cgame call the client sound system. |
| Gameplay helpers | `code/game/g_utils.cpp`, gameplay files under `code/game/` | Starts sounds from gameplay code, creates sound events, sets looping sound state. |
| Entity/event handling | `code/cgame/cg_event.cpp`, `code/cgame/cg_ents.cpp` | Receives events/entity state and turns them into sound playback calls. |
| Sound channels enum | `code/game/channels.h` | Defines `CHAN_AUTO`, `CHAN_WEAPON`, `CHAN_VOICE`, `CHAN_BODY`, and other logical sound channels. |

Multiplayer has a parallel sound tree under `codemp/client/`. The architecture is similar, but this document focuses on the single-player/client sound path in `code/`.

## The Big Mental Model

Think of the sound system as a small mixing desk.

The game says, "play this sound on this entity." The sound system then:

- looks up the sound file;
- picks one of a limited number of playback slots;
- decides how loud it should be in the left and right speaker;
- mixes the sample data into a shared output buffer;
- lets the platform audio backend play that buffer.

The renderer gets a list of things to draw each frame. The audio system gets a list of things to hear each frame, plus one-shot sound events that have just happened.

## Important Concepts

### `sfxHandle_t`

`sfxHandle_t` is a small integer handle for a sound effect. Game and cgame code usually do not hold raw audio data directly. They register a path such as:

```cpp
"sound/weapons/saber/saberhitwall1.wav"
```

and get back a handle. That handle points into the client's known sound table.

The actual loaded sound is stored as an `sfx_t` in `snd_local.h`. An `sfx_t` records things like:

- the sound name;
- decoded sample data;
- whether it is a WAV-style 16-bit sample or MP3-backed data;
- sound length;
- memory/cache state;
- optional lip-sync volume data;
- optional OpenAL buffer state.

### `channel_t`

A `channel_t` is an active playback slot. If a blaster shot is currently playing, it occupies a channel. If a footstep is playing, it occupies a channel. The software mixer uses `MAX_CHANNELS`, which is 32 in `snd_local.h`.

A channel stores:

- which entity owns the sound;
- which logical sound channel it is on, such as weapon, voice, body, or item;
- the left and right speaker volume;
- the master volume;
- whether the sound has a fixed world origin;
- which `sfx_t` is playing;
- where playback started;
- whether it is a looping sound.

In plain terms: `sfx_t` is "what sound is this?" and `channel_t` is "one currently playing instance of that sound."

### Logical Sound Channels

`code/game/channels.h` defines named channels:

- `CHAN_AUTO`: automatically pick an available playback channel.
- `CHAN_LOCAL`: menu/local sounds.
- `CHAN_WEAPON`: weapon sounds.
- `CHAN_VOICE`: voice sounds that can drive mouth animation.
- `CHAN_VOICE_ATTEN`: voice sounds with stronger falloff.
- `CHAN_VOICE_GLOBAL`: voice sounds without normal distance falloff.
- `CHAN_ITEM`: item sounds.
- `CHAN_BODY`: body movement, impacts, falls, and similar sounds.
- `CHAN_AMBIENT`: ambient one-shots.
- `CHAN_LOCAL_SOUND`: chat/UI-style local sounds.
- `CHAN_ANNOUNCER`: announcer-style voice.
- `CHAN_LESS_ATTEN`: falls off less quickly than normal sounds.
- `CHAN_MUSIC`: used for looping music-like playback.

These names are not just labels. They affect replacement and attenuation.

For example, if an entity starts a new `CHAN_WEAPON` sound, the sound system can replace a previous weapon sound on that same entity. This prevents a single actor from stacking too many overlapping weapon sounds. `CHAN_AUTO` is looser: it looks for an open channel and only steals an older channel if needed.

Voice channels also get special handling. Voice sounds can use `s_volumeVoice` rather than the normal `s_volume`, and they can update per-entity mouth/lip-sync data.

## How a One-Shot Sound Effect Plays

A common gameplay path looks like this:

```text
game code
  -> G_SoundOnEnt(...)
  -> cgi_S_StartSound(...)
  -> client/cgame syscall bridge
  -> S_StartSound(...)
  -> S_PickChannel(...)
  -> S_SpatializeOrigin(...)
  -> S_Update(...)
  -> S_PaintChannels(...)
  -> SDL/OpenAL output
```

### 1. Gameplay requests a sound

Gameplay code often calls helpers in `code/game/g_utils.cpp`.

`G_SoundOnEnt` is a direct single-player helper. It:

- converts a path to a sound config index with `G_SoundIndex`;
- updates the client-side position for the entity;
- starts the sound through `cgi_S_StartSound`;
- falls back to custom character sound lookup if needed.

That is used in places like combat, AI, movement, weapons, and saber code:

```cpp
G_SoundOnEnt( self, CHAN_BODY, "sound/weapons/force/jump.wav" );
G_SoundOnEnt( self, CHAN_WEAPON, "sound/weapons/saber/saberhup1.wav" );
G_SoundOnEnt( NPC, CHAN_VOICE, "*anger1.wav" );
```

The game can also create temporary sound events. `G_Sound` creates an `EV_GENERAL_SOUND` temp entity. `G_SoundBroadcast` creates an `EV_GLOBAL_SOUND` temp entity. Cgame later sees those events in `cg_event.cpp` and calls `cgi_S_StartSound`.

### 2. Cgame bridges into the client sound system

Cgame does not directly own the audio device. It calls wrapper functions like:

- `cgi_S_StartSound`
- `cgi_S_StartLocalSound`
- `cgi_S_AddLoopingSound`
- `cgi_S_UpdateEntityPosition`
- `cgi_S_Respatialize`
- `cgi_S_RegisterSound`

Those wrappers cross into the client through `cl_cgame.cpp`, which dispatches cgame syscalls to real sound functions such as `S_StartSound` and `S_RegisterSound`.

This keeps the high-level cgame code separate from the low-level audio implementation.

### 3. `S_StartSound` validates and loads the sound

`S_StartSound` in `snd_dma.cpp` is the central one-shot sound entry point.

It checks:

- sound is initialized and not muted;
- the entity number is valid if no fixed origin was provided;
- the `sfxHandle_t` is in range;
- the referenced `sfx_t` is loaded into memory.

If the sound has not been loaded yet, `S_memoryLoad` calls into `S_LoadSound`.

Then `S_StartSound` picks a playback channel, fills in the channel fields, and marks the sound to start immediately.

### 4. `S_PickChannel` chooses what gets to play

There are only so many active channels. In the software path, `MAX_CHANNELS` is 32. If all are busy, something has to be replaced.

`S_PickChannel` prefers:

- an open channel;
- a channel from the same entity and same logical channel, if the new sound should replace it;
- otherwise an older eligible channel.

It avoids replacing some important sounds, such as listener/player-owned sounds, and avoids stomping looping sounds where possible.

This is why channels matter. A weapon charging sound, a saber swing, a footstep, and an NPC voice are not all treated as identical noise. The logical channel gives the sound system just enough information to make reasonable replacement decisions.

### 5. The sound is spatialized

Spatialization means: "Given the listener's position and facing direction, how loud should this sound be in the left and right speakers?"

The listener is usually the camera/player. Cgame updates it with `S_Respatialize`, passing:

- listener entity number;
- listener origin;
- listener axis/facing directions;
- whether the listener is in water.

Each sound source is either:

- fixed at a world origin, if `S_StartSound` was called with an origin; or
- attached to an entity, if the origin was `NULL`.

For entity-attached sounds, `S_UpdateEntityPosition` keeps the sound system informed of each entity's current origin.

`S_SpatializeOrigin` then computes:

- distance from listener to sound;
- attenuation from that distance;
- left/right panning from the listener's right axis;
- special attenuation behavior for voice and less-attenuated channels.

Close sounds play at full volume. Farther sounds fade. Sounds to the listener's left become louder in the left speaker; sounds to the right become louder in the right speaker.

This is simple stereo panning and distance attenuation. It is not geometric acoustic simulation. The code does not trace around walls, calculate reverb from room shape, or simulate real sound propagation.

## How Looping Sounds Work

Looping sounds are used for things like:

- saber hum;
- force power loops;
- mover/door machinery;
- turret rotation;
- vehicle engine loops;
- projectile loops;
- environmental hums.

There are two common ways a loop appears.

### Entity `loopSound`

`entityState_t` has a `loopSound` field. Gameplay can set:

```cpp
ent->s.loopSound = G_SoundIndex( "sound/weapons/force/lightning2.wav" );
```

or clear it:

```cpp
ent->s.loopSound = 0;
```

Cgame reads this in `CG_EntityEffects` in `cg_ents.cpp`. If the entity has a loop sound and is not hidden, cgame calls `cgi_S_AddLoopingSound` for that frame.

### Per-frame loop list

The important detail is that continuous looping sounds must be re-added every frame before `S_Update`.

`S_ClearLoopingSounds` clears the frame's loop list. Cgame then adds currently audible loops with `S_AddLoopingSound`. During the sound update, `S_AddLoopSounds` turns that loop list into active mixer channels.

This means a loop is not simply "started once and forgotten." The game/cgame state says what should be looping right now, and the sound system rebuilds the active loop contribution each frame.

`S_AddLoopSounds` can also merge duplicate loop sounds. If several sources use the same sample, it sums their left/right volume contributions and plays the sample once with the combined volume. That is a cheap way to avoid wasting channels on identical loops.

One limitation: streamed MP3 sounds cannot be used for normal looping sounds in this path. The loop mixer needs random access into the sample data. Streamed MP3 data is sequential, so `S_AddLoopSounds` errors if a streamed MP3 is used there.

## How Local Sounds Work

Local sounds are sounds that should be heard by the player without normal world positioning.

Examples include:

- UI clicks;
- no-ammo beeps;
- menu focus sounds;
- messages;
- some player feedback sounds.

`S_StartLocalSound` simply starts a sound on the listener entity. Since it is tied to the listener, it plays like a local/player sound rather than like a distant world object.

## Asset Loading and Caching

Sound registration is done through `S_RegisterSound`.

The public comment says registration always tries to return a valid sample or placeholder so the game does not keep hitting the filesystem for a missing file. In this Raven version, if the load marks the sound as default/missing, callers often receive `0`, and many paths treat that as "do not play" or try a custom fallback.

`S_LoadSound` in `snd_mem.cpp` handles the file work:

- normalizes the filename;
- adds `.wav` if no extension is present;
- tries WAV first;
- tries MP3 if WAV is missing;
- applies voice language substitutions such as `chars` to localized voice directories;
- falls back to English voices if localized files are missing;
- parses PCM WAV data;
- validates MP3 data with the in-tree MP3 code;
- resamples decoded data to the current output rate;
- tracks maximum sample volume for lip-sync support.

WAV loading is intentionally narrow. `GetWavinfo` expects RIFF/WAVE PCM. Unsupported WAV formats are rejected.

MP3 support is split:

- some MP3s are unpacked into sample data and then treated like normal sound effects;
- some character/voice MP3s can stay as streaming MP3 data;
- background music and cinematics use separate raw/streaming paths.

The current output rate comes from the sound device. `ResampleSfx` converts loaded samples to match `dma.speed`, so the mixer can combine all active sounds in the same sample rate.

## The Per-Frame Update

`S_Update` is called every frame from the client.

At a high level it:

1. Updates background track/music state.
2. Adds/refreshes looping sounds.
3. Re-spatializes active channels if the listener moved.
4. Determines how far ahead to mix.
5. Calls `S_PaintChannels` to generate samples.
6. Submits/unlocks the audio buffer for the platform backend.

The sound system mixes slightly ahead of the current playback position. That gives the hardware/OS audio callback enough buffered data to play without underrunning. Cvars such as `s_mixahead` and `s_mixPreStep` affect this timing.

## The Software Mixer

The main software mixer is in `snd_mix.cpp`.

It uses a temporary stereo `paintbuffer`. Each paint step:

1. Clears the paintbuffer to silence, or starts it with raw streaming samples such as music/cinematics.
2. Iterates over active `s_channels`.
3. Skips channels with no sound or no audible volume.
4. Chooses normal volume or voice volume.
5. Reads the sound sample data.
6. Scales it by the channel's left/right volume.
7. Adds it into the paintbuffer.
8. Clamps and transfers the result to the DMA output buffer.

For ordinary decoded sounds, `S_PaintChannelFrom16` mixes 16-bit sample data. For MP3-backed sounds, `S_PaintChannelFromMP3` asks the MP3 stream code for decoded samples and then mixes them the same way.

The final output is just a stream of audio samples. The mixer does not know about lightsabers, doors, stormtroopers, or UI buttons by this point. It only sees active channels with sample data and left/right volumes.

## SDL Output

On the normal portable path, `shared/sdl/sdl_sound.cpp` opens an SDL audio device.

`SNDDMA_Init` chooses:

- sample rate from `s_khz` through `SNDDMA_ExpandSampleFrequencyKHzToHz`;
- sample bits from `s_sdlBits`;
- channel count from `s_sdlChannels`;
- device/mixer buffer sizes from SDL and cvars.

It allocates `dma.buffer`, starts SDL's audio callback, and records the device format in `dma_t`.

The game mixer writes into `dma.buffer`. SDL's `SNDDMA_AudioCallback` copies bytes from that buffer into the stream requested by the OS audio device. The code uses `SNDDMA_BeginPainting` and `SNDDMA_Submit` to lock/unlock the SDL audio device while the mixer updates the buffer.

So the final flow is:

```text
active channels
  -> paintbuffer
  -> dma.buffer
  -> SDL audio callback
  -> operating system audio device
  -> speakers/headphones
```

## Legacy OpenAL Path

There is also a legacy OpenAL path behind `USE_OPENAL`.

In this tree, `USE_OPENAL` is only enabled for a narrow old Windows/MSVC configuration, and it carries old EAX/environment code. When enabled, OpenAL can own hardware/software sources and buffers instead of using the SDL software mixer in the same way.

For most current OpenJK development, the SDL/software path is the practical path to understand first. The OpenAL code is historically important but not the clean modern abstraction it might sound like from the name.

## Sound Effects vs Music vs Ambient

The code uses the same word "sound" for several different things, but they are not all handled identically.

### Game sound effects

These are the main subject of this document:

- footsteps;
- impacts;
- weapon fire;
- saber swings;
- NPC barks;
- item pickups;
- door/mover sounds;
- force power sounds.

They usually go through `S_RegisterSound`, `S_StartSound`, `S_AddLoopingSound`, and the channel mixer.

### Background music and raw samples

Music uses background track and raw sample paths:

- `S_StartBackgroundTrack`
- `S_StopBackgroundTrack`
- `S_UpdateBackgroundTrack`
- `S_RawSamples`

`S_RawSamples` feeds decoded streaming data into `s_rawsamples`. The mixer begins each paintbuffer with those raw samples, then mixes game sound effect channels on top.

This is why music is not usually "just another footstep channel." It is treated more like a continuous raw stream underneath the sound effects.

### Ambient systems

`snd_ambient.cpp` contains additional ambient set logic. Ambient one-shots and ambient loops can still land in the channel/looping systems, but ambient selection and environmental behavior live outside the basic one-shot sound path.

## Voice, Custom Sounds, and Lip Sync

Character sounds often use names beginning with `*`, such as:

```cpp
"*pain100.wav"
"*anger1.wav"
"*jump1.wav"
```

Those are custom character sounds. Instead of being loaded directly as a literal file path, they are resolved through cgame custom sound logic such as `CG_TryPlayCustomSound`. That lets different NPCs or player models map the same logical voice event to different actual files.

Voice channels are also special because they can drive mouth animation. The loader tracks volume information (`fVolRange` and optional lip-sync buffers), and voice playback updates per-entity sound volume data. The animation side can use that to make mouths move with speech-like audio intensity.

This is a practical approximation. It is not phoneme recognition. It is closer to "how loud is this voice sample right now?"

## Raytraced Sound and Lightsaber Hums

It is technically possible to shoot rays for a sound source such as a lightsaber hum. A saber hum is just a sound source attached to an entity or looped from entity state, so code could take:

```text
listener position
lightsaber/source position
world collision geometry
```

and run ray or trace tests between them.

For example, a simple test could ask:

```text
Can a trace go straight from the listener to the saber?
Is there a wall between them?
How many test rays reach the listener?
Does the source appear to be around a corner?
```

If the only desired result is "make the saber quieter when blocked by a wall," that is plausible within this engine. The ray result could become an occlusion multiplier:

```text
open path:       volume *= 1.0
partly blocked:  volume *= 0.6
fully blocked:   volume *= 0.25
```

That fits the current audio architecture because the mixer already understands left and right volume.

The problem is not that a lightsaber hum cannot be traced. The problem is that the current sound system has very little place to put the richer result of a real acoustic raytrace.

The current path for a looped saber hum is roughly:

```text
saber/player entity
  -> entity loopSound or cgame looping sound
  -> S_AddLoopingSound
  -> S_AddLoopSounds
  -> channel_t
  -> left/right volume
  -> S_PaintChannels
  -> final mixed samples
```

By the time the sound reaches `S_PaintChannels`, the mixer mostly sees:

```text
sample data + left volume + right volume
```

It does not have built-in per-sound data for:

- wall material absorption;
- high-frequency muffling;
- reflected sound paths;
- late reverb tails;
- diffraction around corners;
- separate "direct" and "reflected" versions of the same sound;
- per-channel filter state.

So a ray system could easily answer more detailed acoustic questions than the existing mixer can use.

### Why looping sounds are extra awkward

Looping sounds have another wrinkle: `S_AddLoopSounds` may merge duplicate loop sounds that use the same `sfx_t`. It spatializes matching loops, sums their left/right volume totals, and plays one combined loop.

That is efficient for the original engine design, but it works against detailed raytraced acoustics. Two saber hums using the same sample might need different occlusion, reflection, or muffling results. If they are merged too early, those separate acoustic identities are lost.

For simple volume-only occlusion, merging is less of a problem because the code can sum reduced volumes. For anything richer than volume, such as a muffled saber behind a wall and an unmuffled saber in the open, the system would need to preserve separate loop/channel state instead of collapsing them into one combined sample.

### What would be realistic to add

A practical first step would be fake raytraced occlusion, not full raytraced sound.

That means adding fields such as:

```cpp
float occlusion;
float lowpassAmount;
```

to active sound or loop/channel state, then computing those values from traces between the listener and the sound source.

The easiest version would only adjust volume:

```text
trace blocked -> reduce left/right volume
trace open    -> leave left/right volume alone
```

A more advanced version could apply a low-pass filter so blocked sounds lose high frequencies and feel muffled. That would require mixer changes because the current `S_PaintChannelFrom16` and `S_PaintChannelFromMP3` paths just scale samples by volume and add them into the paintbuffer.

True raytraced sound would be a much larger feature. It would need new concepts the current audio system does not really have:

- multiple acoustic paths per source;
- reflected/delayed copies of sounds;
- per-surface material data;
- per-channel filtering;
- reverb/echo accumulation;
- persistent acoustic state across frames;
- probably a different loop handling model.

So for a lightsaber hum:

- **Ray-tested volume occlusion:** realistic to add.
- **Ray-tested muffling:** possible, but needs mixer/filter work.
- **True raytraced saber acoustics with reflections and room response:** major audio-system rewrite.

## What the System Does Not Do

This audio architecture is simple and fast, but limited.

It does not provide:

- authored audio graphs;
- real-time acoustic ray tracing;
- wall occlusion by default;
- per-room convolution reverb;
- physical sound diffraction;
- high-level bus routing like modern middleware;
- automatic prioritization based on designer-authored sound classes;
- arbitrary numbers of simultaneous sounds.

Distance and stereo panning are the main spatial effects. The optional old OpenAL/EAX path can add environmental behavior on supported legacy setups, but the core portable path is a software stereo mixer.

## Common Practical Questions

### Why did my sound not play?

Likely causes:

- the sound failed to register or returned handle `0`;
- the file is missing or the extension fallback did not find it;
- the entity number is invalid;
- sound is muted or not initialized;
- all channels are busy and the sound was replaced quickly;
- the sound is spatialized to zero volume because it is too far away;
- a looping sound was not re-added this frame;
- a streamed MP3 was used where random-access loop sample data is required.

### Why does a new sound cut off an old one?

The active channel pool is limited. Also, entity/channel pairs intentionally replace each other. If the same entity starts a new `CHAN_WEAPON` sound, replacing the previous weapon sound is often expected behavior.

Use `CHAN_AUTO` when replacement is not desired, but remember that `CHAN_AUTO` still competes for the finite active channel pool.

### Why can some sounds follow entities?

If `S_StartSound` receives `origin == NULL`, the sound is dynamically sourced from the entity number. The sound system uses the latest position supplied by `S_UpdateEntityPosition`.

If `S_StartSound` receives an explicit origin, the sound is fixed at that world point.

### Why are looping sounds added every frame?

Because the loop list represents the current frame's audible persistent sounds. If an entity stops looping, cgame simply stops adding that loop. The sound system's next update no longer has it in the loop list.

This design keeps loops synchronized with entity state without requiring every caller to manage long-lived audio objects manually.

## Navigation Guide

To understand or change a specific part of the audio system:

- Start a normal sound: `S_StartSound` in `code/client/snd_dma.cpp`.
- Register/load a sound: `S_RegisterSound` in `code/client/snd_dma.cpp`, then `S_LoadSound` in `code/client/snd_mem.cpp`.
- Understand sound replacement: `S_PickChannel` in `code/client/snd_dma.cpp`.
- Understand distance and panning: `S_SpatializeOrigin` and `S_Respatialize` in `code/client/snd_dma.cpp`.
- Understand loops: `S_ClearLoopingSounds`, `S_AddLoopingSound`, and `S_AddLoopSounds` in `code/client/snd_dma.cpp`.
- Understand final mixing: `S_PaintChannels` in `code/client/snd_mix.cpp`.
- Understand device output: `SNDDMA_Init` and `SNDDMA_AudioCallback` in `shared/sdl/sdl_sound.cpp`.
- Understand gameplay sound helpers: `G_SoundOnEnt`, `G_Sound`, and `G_SoundBroadcast` in `code/game/g_utils.cpp`.
- Understand sound events: `EV_GENERAL_SOUND` and `EV_GLOBAL_SOUND` handling in `code/cgame/cg_event.cpp`.
- Understand entity looping state: `CG_EntityEffects` in `code/cgame/cg_ents.cpp`.

## End-to-End Example: Saber Swing

A saber swing sound can be requested from gameplay code, often on `CHAN_WEAPON`.

The path is:

1. Weapon/saber code asks for a swing sound on the saber or player entity.
2. The game helper resolves the sound path/index.
3. Cgame/client starts the sound with `S_StartSound`.
4. `S_PickChannel` may replace the previous weapon sound on that same entity.
5. The sound is attached to the entity if no fixed origin is passed.
6. Each frame, listener position is updated.
7. The sound is spatialized based on the listener and entity position.
8. `S_PaintChannels` mixes the saber sample into the stereo output.
9. SDL's audio callback sends the mixed samples to the OS.

To a player, this feels like "the saber made a swing noise." Internally, it is a handle, an entity number, a logical channel, a channel slot, two speaker volumes, and decoded sample data being mixed into a ring buffer.
