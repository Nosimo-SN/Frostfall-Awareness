# Frostfall Awareness

A [SkyrimNet](https://fateless.ai) add-on for [Frostfall](https://www.nexusmods.com/skyrimspecialedition/mods/671)
that makes the cold part of the conversation. The people around you know how cold it is, notice when you're
freezing, can pull you close to share body heat, and remember carrying you to safety when Frostfall rescues
you from exposure.

It has three parts, installed together by a FOMOD:

| Part | What it does |
|---|---|
| **SkyrimNet plugin** (`nosimo.frostfall_awareness`) | Two prompts: the air temperature for everyone in the scene, and a *Cold Exposure* section for each character who is feeling the cold. Two actions: *Share Body Heat* and *Warm Each Other*. A settings page. |
| **SKSE plugin** (`Frostfall_Awareness.dll`) | Narrates exposure and frostbite, writes rescue memories, plays the hug and warm glow, frosts over cold NPCs, warms you during intimate scenes, and supplies the eligibility checks for the actions. No Papyrus scripts, no game plugin. |
| **Cold Shiver animations** (optional) | Shivering idle and walk animations through Open Animation Replacer, by distar66 (see Credits). |

## Requirements

- Skyrim SE / AE (1.5.97 and 1.6.x) with [SKSE](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- Frostfall 3.4 or Frostfall 2026 (`Frostfall.esp`)
- SkyrimNet Beta 28 or later (Beta 29 keeps an action's target when the NPC adds a thought after it,
  which helps *Warm Each Other*)
- For the Cold Shiver animations: [Open Animation Replacer](https://www.nexusmods.com/skyrimspecialedition/mods/92109)

Optional, picked up when present: [Advanced Nudity Detection](https://www.nexusmods.com/skyrimspecialedition/mods/165289)
(revealing clothing counts as bare to the cold), SeverActions (its followers count as rescuers, and its own
follower survival takes over their cold), SexLab or OStim (scene warmth).

## Installation

Install the archive with your mod manager and pick the options in the installer. No game plugin is added, so
removing the mod is safe. If another mod already gives you cold animations (shivering idles or cold
walks, such as (DMC) Shiver Alpha), use either those or the Cold Shiver option, not both: Open Animation
Replacer plays one animation at a time, picked by priority, so the two would compete.

## What it does

Nothing is described or posted while you are comfortable: the cold only enters the story once Frostfall's
debuffs apply (Chilly and worse, or frostbite).

**Air temperature.** Everyone in the scene is told the air temperature where you are, in Frostfall's own ten
Weathersense tiers from *hot* to *frigid and deadly*, indoors and out. Characters can make small talk about
the weather, and NPCs whose cold another mod tracks still know the air.

**Cold Exposure.** For you, read from Frostfall: how cold you are, wetness, how much your clothing and any
warming spell help, fire and shelter, and frostbite. For NPCs outdoors near you, an estimate from the same air
and the garment on their body (A.N.D. revealing outfits count as bare; 50+ frost resistance holds off one
step), never more than one step colder than you, because Frostfall's exposure builds over time and an
estimate does not. The prompts state facts; how characters react is up to them.

**Narration.** When a condition starts (only when getting colder) a short line is narrated to the people
around you, and repeated at the interval you set while it lasts. Each can also prompt a thought of your own.
Nothing is posted in combat, shortly after it, or while a conversation is going on.

**Share Body Heat.** A friendly person standing close by may choose to hold you close. It removes 10
exposure through Frostfall's own exposure function and never takes you below Chilly, so only fire or shelter
truly warms you. They pull you into the vanilla hug and a warm glow passes over you both.

**Warm Each Other.** NPCs may do the same for each other when someone near them is feeling the cold.
Frostfall only tracks you, so this is the gesture alone: the hug, the glow, and the frost leaving them.

**Frost on NPCs.** NPCs outdoors who are feeling the cold get Frostfall's frost look, the lighter frost at
Freezing and the full one at Freezing to Death. It fades within seconds once they warm up, go indoors or you
leave.

**Rescues.** With Frostfall's *Max Exposure* set to rescue, the companion who saved you gets a first-person
memory of carrying you in from the cold, you get a hazy one of blacking out, the room sees you brought in, and
your rescuer is the first to speak when you wake.

**Scene warmth.** During a SexLab or OStim scene, shared body heat removes a little exposure every ten
seconds.

**Vampires and the undead** feel no cold by default; each has its own switch. **NPC Cold Shift** makes NPCs
hardier or quicker to feel the cold by whole steps.

**Cold Shiver** (optional). You shiver from Frostfall's Very Cold on; NPCs outdoors shiver while you are at
least Chilly and the air is cold enough for what they wear. Open Animation Replacer can't read SkyrimNet
settings, so the animations always leave out vampires and the undead and ignore the cold shift. Children
never shiver.

## Settings

Everything is on the plugin's page in SkyrimNet's dashboard and applies within about ten seconds:
narration on/off per condition, repeat intervals, narration texts, rescue options, Share Body Heat / Warm
Each Other with their hug and glow, frost on NPCs, NPC cold shift, vampires and undead, scene warmth, and a
debug log. The two actions can be given cooldowns in SkyrimNet's action library.

The SKSE plugin logs to `Documents/My Games/Skyrim Special Edition/SKSE/Frostfall_Awareness.log`.

## Building

Requirements: Visual Studio 2022 (C++ workload), CMake 3.21+, Ninja, and [vcpkg](https://github.com/microsoft/vcpkg)
(`VCPKG_ROOT` set).

```bash
git clone --recursive https://github.com/Nosimo-SN/Frostfall-Awareness.git
cd Frostfall-Awareness
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build
python tools/package.py . 1.0.0
```

Run the CMake commands from a VS 2022 x64 developer prompt. `tools/package.py` writes the FOMOD archive to
`build/dist/`. `tools/gen_settings.py` generates the settings page and `tools/gen_oar.py` the animation
configs (from an installed copy of Shiver Alpha).

## Credits

- **distar66** for the Shiver Alpha animations (GPL v3), used unchanged.
- **Chesko** and **CageTV** for Frostfall and Frostfall 2026.
- The SkyrimNet team (Fateless.Ai) for SkyrimNet and its plugin API.
- CommonLibSSE-NG and the libraries listed in `THIRD_PARTY_NOTICES.md`.

## License

GPL-3.0-or-later (see `LICENSE`). Third-party components and their licences are listed in
`THIRD_PARTY_NOTICES.md`.
