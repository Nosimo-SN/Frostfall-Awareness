# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Frostfall Awareness.
import json
import sys

out_path = sys.argv[1]

states = [
    ("chilly", "Chilly", 480, "{name} shivers as the cold begins to sap {their} strength.", "Exposure level Chilly: -10% health, stamina and magicka regeneration."),
    ("very_cold", "Very Cold", 240, "{name} is shivering hard, fingers clumsy with cold.", "Exposure level Very Cold: -25% regeneration, -10 to skills."),
    ("freezing", "Freezing", 120, "{name} is shaking uncontrollably, limbs stiff and slow with cold.", "Exposure level Freezing: -50% regeneration, -30 to skills, slowed, frostbite risk."),
    ("freezing_to_death", "Freezing to Death", 60, "{name} stumbles through the cold, lips blue, barely able to move.", "Exposure level Freezing to Death (and max exposure): -75% regeneration."),
    ("frostbite_feet", "Frostbite (Feet)", 240, "{name} walks stiffly on frostbitten feet.", "Frostbitten feet: -10 Sneak, slower movement."),
    ("frostbite_hands", "Frostbite (Hands)", 240, "{name}'s fingers are numb and swollen with frostbite.", "Frostbitten hands: weapon, block, lockpicking and pickpocket penalties."),
    ("frostbite_body", "Frostbite (Body)", 120, "Frostbite has bitten deep into {name}'s body.", "Frostbitten body: -25% regeneration."),
    ("frostbite_head", "Frostbite (Head)", 60, "{name}'s face and ears are raw and white with frostbite.", "Frostbitten face and ears: Speech and magic penalties."),
]


def q(s):
    # JSON string literals are valid YAML double-quoted scalars.
    return json.dumps(s, ensure_ascii=False)


out = []
w = out.append


def field(name, path, typ, desc, default, cat, bounds=None):
    w(f"    - name: {q(name)}")
    w(f"      path: {q(path)}")
    w(f"      type: {q(typ)}")
    w(f"      description: {q(desc)}")
    if typ == "bool":
        w(f"      defaultValue: {'true' if default else 'false'}")
    elif typ in ("float", "int"):
        w(f"      defaultValue: {default}")
    else:
        w(f"      defaultValue: {q(str(default))}")
    if bounds:
        w(f"      min: {bounds[0]}")
        w(f"      max: {bounds[1]}")
    w(f"      category: {q(cat)}")
    w("")


w("plugin:")
w('  name: "Frostfall Awareness"')
w('  version: "1.0.1"')
w("  description: >-")
w("    Turns Frostfall's cold into something the people around you notice. While Frostfall applies its")
w("    exposure or frostbite penalties to you, the Frostfall_Awareness SKSE plugin posts a short narration")
w("    line when a condition starts and repeats it at the interval you set while it lasts. Nothing is posted")
w("    in combat or while a conversation is going on. When Frostfall rescues you from exposure, the people")
w("    who saved you remember it, and you keep a hazy memory of blacking out. Changes apply within about ten")
w("    seconds, no restart needed.")
w('  icon: "❄️"')
w("")
w("schema:")
w("  fields:")
w("    # --- General ---------------------------------------------------------------------------------")
field("Enable Cold Narration", "general.enabled", "bool",
      "Master switch for exposure and frostbite narration. Rescue memories have their own switch below.", True, "General")
field("Force A Reaction", "general.force_reaction", "bool",
      "On: posted as direct narration, so someone nearby reacts right away. Off: logged as a quiet scene event that people know about and may bring up later.",
      True, "General")
field("Your Own Thought", "general.player_thought", "bool",
      "Narration only reaches the people around you. On: each cold narration also prompts a thought of your character's own, which SkyrimNet shows and voices like any player thought, so you feel the cold too.",
      True, "General")
field("Minimum Gap (seconds)", "general.min_gap", "int",
      "After any cold narration, nothing else is narrated for this long, so several conditions don't pile up.", 60, "General", (0, 600))
field("Quiet After Dialogue (seconds)", "general.dialogue_quiet", "int",
      "Wait until no SkyrimNet dialogue has happened for this long, so narration doesn't crowd a conversation.", 15, "General", (0, 120))
field("Quiet After Combat (seconds)", "general.combat_grace", "int",
      "Nothing is narrated in combat (Frostfall pauses exposure there anyway) or for this long after it ends.", 10, "General", (0, 120))
w("    # --- Conditions ------------------------------------------------------------------------------")
for key, label, rep, text, desc in states:
    field(f"Narrate: {label}", f"narrate.{key}", "bool",
          desc + " Narrated when it starts (only when getting colder, never when warming up).", True, "Conditions")
    field(f"Repeat: {label} (seconds)", f"repeat.{key}", "int",
          "Narrate again every this many seconds while it lasts. 0 = only when it starts.", rep, "Conditions", (0, 1200))
w("    # --- Narration text --------------------------------------------------------------------------")
for key, label, rep, text, desc in states:
    field(f"Text: {label}", f"text.{key}", "string",
          "Placeholders: {name} = your character, {their} = his/her, {they} = he/she, {them} = him/her. Describe what can be seen; let the people around you decide how to react.",
          text, "Narration Text")
w("    # --- Rescue ----------------------------------------------------------------------------------")
field("Rescue Memories", "rescue.memories", "bool",
      "When Frostfall rescues you (Max Exposure set to rescue), the companions who saved you get a first-person memory of it, recruited creatures and animals included, each in its own way. When SkyrimNet keeps memories to actors capable of speech (its Memory Filter setting), creatures it doesn't count as speaking get none. Only Frostfall's own rescue counts: if another mod takes over the collapse, nothing is written. One LLM call per rescue.",
      True, "Rescue")
field("Your Hazy Memory", "rescue.player_memory", "bool",
      "You also get a memory: a hazy recollection of blacking out and glimpses of your rescuer.", True, "Rescue")
field("Rescue Event", "rescue.event", "bool",
      "Also log a one-line scene event about the rescue, so people around you know what happened.", True, "Rescue")
field("The Room Sees You Brought In", "rescue.arrival_line", "bool",
      "As you come to, the people around you are told your rescuer has carried you in from the cold, so the innkeeper or patrons can react. Your rescuer's own line follows a few seconds later. Only when a companion or creature brought you somewhere new.",
      True, "Rescue")
field("Rescuer Speaks When You Wake", "rescue.wake_line", "bool",
      "Once you are back on your feet and the room is quiet, your waking is narrated to your rescuer alone, so they are the one who speaks to you, already remembering the rescue. A person rescuer speaks first; a creature only when it got a memory and SkyrimNet lets actors not capable of speech take part (its Actor Filter setting).",
      True, "Rescue")
field("Quiet After Rescue (seconds)", "rescue.quiet", "int",
      "After a rescue, no cold narration for this long, so waking up isn't crowded. Conditions you already had on the mountain (and the frostbite Frostfall rolls as you wake) are not announced again; their repeats count from the rescue.",
      180, "Rescue", (0, 600))
field("Stop Dialogue On Collapse", "rescue.purge_dialogue", "bool",
      "Stop all queued and playing NPC dialogue (SkyrimNet's purge) twice: when Frostfall's rescue is confirmed, so lines started on the mountain don't follow you, and again as you dismiss the rescue message, so nothing generated during the blackout plays over your waking. You are unconscious in between anyway.",
      True, "Rescue")
w("    # --- Share Body Heat ---------------------------------------------------------------------------")
field("Share Body Heat Action", "huddle.enabled", "bool",
      "Lets friendly people standing close to you (about 10 metres) huddle with you and share body heat while you are Chilly or worse, outside combat; the AI decides whether that character would. Each huddle removes 10 exposure through Frostfall's own exposure function and never takes you below Chilly, so only fire or shelter truly warms you. The action is FrostfallShareBodyHeat in SkyrimNet's action library, where you can also give it a cooldown.",
      True, "Share Body Heat")
field("Warm Each Other", "huddle.npc_enabled", "bool",
      "Lets people near you warm each other too: someone may pull a cold companion or bystander close, with the hug and glow below. Frostfall only tracks you, so it is the gesture alone, though the warmed pair lose their frost look for five minutes. The action is FrostfallWarmEachOther in SkyrimNet's action library.",
      True, "Share Body Heat")
field("Hug Animation", "huddle.hug", "bool",
      "When someone shares body heat, they pull you into the vanilla hug. It may not start if either of you is sitting, riding or mid-move; the warming applies anyway.",
      True, "Share Body Heat")
field("Warm Glow", "huddle.glow", "bool",
      "A brief warm glow (the resist-fire potion shimmer) on both of you as the heat is shared.",
      True, "Share Body Heat")
w("    # --- NPC Frost --------------------------------------------------------------------------------")
field("Frost On NPCs", "frost.npc_shader", "bool",
      "NPCs outdoors who are feeling the cold get Frostfall's frost look, like yours: the lighter frost at Freezing, the full one at Freezing to Death. Their cold follows yours (one step colder in revealing clothing, per A.N.D.), the air, and 50+ frost resistance, the same rules as the optional Cold Shiver animations. The frost fades within seconds once they warm up, go indoors or you leave, or when this is switched off.",
      True, "NPC Frost")
w("    # --- NPC Cold ---------------------------------------------------------------------------------")
field("NPC Cold Shift", "npc.cold_shift", "int",
      "How readily NPCs feel the cold, in whole steps of air temperature: -2 = much hardier, 0 = default, +2 = they feel it much sooner. Applies to how NPCs are described to the AI, their frost look and who can be warmed; the optional Cold Shiver animations keep the default.",
      0, "NPC Cold", (-2, 2))
field("Vampires Feel No Cold", "npc.vampires_immune", "bool",
      "Vampire NPCs (Serana until she is cured, Valerica, anyone with the Vampire keyword or in a vampire faction, vampire followers) are never described as cold, never frost over and are never in need of warming, and with no body heat of their own they never offer to share it (Share Body Heat, Warm Each Other). The optional Cold Shiver animations always leave vampires out.",
      True, "NPC Cold")
field("Undead Feel No Cold", "npc.undead_immune", "bool",
      "Undead NPCs (ghosts, liches, undead followers) are never described as cold, never frost over, are never in need of warming and never offer their body heat. Some mods also mark vampires as undead; this switch covers those vampires too, even if Vampires Feel No Cold is off. The optional Cold Shiver animations always leave the undead out.",
      True, "NPC Cold")
w("    # --- Heat Sources ------------------------------------------------------------------------------")
field("Recognise Copied Fires", "heat.match_copies", "bool",
      "Frostfall only recognises fires by their exact record. DynDOLOD and some mods place copies of vanilla fires under new records (giant camp fires are the usual case), and Frostfall then ignores them. On: at startup, every object that uses the same model as a fire Frostfall knows is counted as that fire. Takes effect the next time you start the game.",
      True, "Heat Sources")
w("    # --- Scene Warmth ------------------------------------------------------------------------------")
field("Warmth From Intimate Scenes", "scene.enabled", "bool",
      "While you are in a SexLab or OStim scene (cuddles included), shared body heat warms you a little every 10 seconds through Frostfall's own exposure function. Either framework is optional; nothing happens without them.",
      True, "Scene Warmth")
field("Warmth Per 10 Seconds", "scene.rate", "float",
      "Exposure removed every 10 seconds of a scene, on Frostfall's 0-120 scale (each exposure level is 20). 0 = none. For comparison, one Share Body Heat huddle removes 10 once.",
      0.1, "Scene Warmth", (0.0, 3.0))
w("    # --- Debug -----------------------------------------------------------------------------------")
field("Log All Changes", "debug.log_all", "bool",
      "Write every condition start and end to Frostfall_Awareness.log (next to skse64.log).", False, "Debug")

with open(out_path, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(out).rstrip() + "\n")
print(f"{len(out)} lines")
