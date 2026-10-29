# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Frostfall Awareness.
"""Writes the Cold Shiver OAR configs and copies the Shiver Alpha clips (distar66, GPL v3) beside them.

Usage: python -I gen_oar.py <shiver mod folder> <output root>
The output root receives meshes/actors/character/animations/OpenAnimationReplacer/Frostfall Awareness/...
"""
import json
import shutil
import sys
from pathlib import Path

shiver_root = Path(sys.argv[1])
out_root = Path(sys.argv[2])

OAR = out_root / "meshes/actors/character/animations/OpenAnimationReplacer/Frostfall Awareness"
SUB = OAR / "Cold Shiver"
SRC = shiver_root / "meshes/actors/character/animations/DynamicAnimationReplacer/_CustomConditions/4000"

# Above Smooth Moveset (83030003), Drunk (6002107), the OSL aroused walk (6002103) and Aroused Sexy Idles
# (9001000+); below Dynamic Sprint Stop (94040000), BFCO, DD (2100000001+), DFFMA modesty (2123100970+).
PRIORITY = 90000100

V = "1.0.0.0"


def form(plugin, fid):
    return {"pluginName": plugin, "formID": fid}


def cond(name, negated=False, version=V, **params):
    c = {"condition": name, "requiredVersion": version}
    if negated:
        c["negated"] = True
    c.update(params)
    return c


def multi(name, *children, negated=False):
    c = cond(name, negated)
    c["Conditions"] = list(children)
    return c


def global_cmp(plugin, fid, op, value):
    return cond("CompareValues", **{"Value A": {"form": form(plugin, fid)}, "Comparison": op, "Value B": {"value": float(value)}})


def exposure_level(op, value):  # Update.esm FrostfallExposureLevelReadOnly, the player's exposure level (-1..6)
    return global_cmp("Update.esm", "CC0E06", op, value)


def air(op, value):  # Update.esm FrostfallCurrentTemperatureReadOnly, Weathersense temperature where the player is
    return global_cmp("Update.esm", "CC0E07", op, value)


def frost_resist(op, value):  # actor value 43 = ResistFrost
    return cond("CompareValues", **{"Value A": {"actorValue": 43, "actorValueType": "Value"}, "Comparison": op, "Value B": {"value": float(value)}})


def is_player(negated=False):
    return cond("IsActorBase", negated, **{"Actor base": form("Skyrim.esm", "7")})


def rainy():  # CurrentWeatherHasFlag: 0 pleasant, 1 cloudy, 2 rainy, 3 snow
    return cond("CurrentWeatherHasFlag", version="1.2.0.0", Flag={"value": 2.0})


AND_REVEALING = [  # Advanced Nudity Detection.esp factions, rank 1 = showing
    ("831", "AND_NudeActorFaction"),
    ("832", "AND_ToplessFaction"),
    ("833", "AND_BottomlessFaction"),
    ("82F", "AND_ShowingChestFaction"),
    ("834", "AND_ShowingBraFaction"),
    ("830", "AND_ShowingGenitalsFaction"),
    ("82E", "AND_ShowingAssFaction"),
    ("835", "AND_ShowingUnderwearFaction"),
]


def revealing():
    return multi("OR", *[
        cond("FactionRank", Faction=form("Advanced Nudity Detection.esp", fid), Comparison="==", **{"Numeric value": {"value": 1.0}})
        for fid, _ in AND_REVEALING
    ])


player_branch = multi("AND",
    is_player(),
    exposure_level(">=", 3),  # Very Cold or worse
)

# NPCs: Frostfall only tracks the player, so their cold is estimated from the same air, outdoors, while the player
# is at least Chilly (the party feels the cold together; matches the prompt's cap). Revealing clothing (A.N.D.)
# shivers sooner; 50+ frost resistance shifts everyone one band colder.
npc_branch = multi("AND",
    is_player(negated=True),
    cond("IsChild", negated=True),
    # Vampires and the undead feel no cold (the DLL and prompt make this switchable; OAR can't read settings).
    cond("HasKeyword", negated=True, Keyword={"form": form("Skyrim.esm", "A82BB")}),   # Vampire
    cond("HasKeyword", negated=True, Keyword={"form": form("Skyrim.esm", "13796")}),   # ActorTypeUndead
    cond("IsInInterior", negated=True),
    exposure_level(">=", 2),
    multi("OR",
        multi("AND", revealing(), frost_resist("<", 50), multi("OR", air("<", 1), multi("AND", rainy(), air("<", 6)))),
        multi("AND", revealing(), air("<", -9)),
        multi("AND", frost_resist("<", 50), air("<", -9)),
        air("<", -14),
    ),
)

submod = {
    "name": "Cold Shiver",
    "description": (
        "Shivering idle and walk while cold. The player shivers from Frostfall's Very Cold on; NPCs outdoors shiver "
        "while the player is at least Chilly and the air is cold enough for what they wear: revealing clothing "
        "(A.N.D.) below 1 (below 6 in rain), anyone below -9, and 50+ frost resistance one band colder. Vampires and the undead never shiver. "
        "Animations: (DMC) Shiver Alpha by distar66, GPL v3."
    ),
    "priority": PRIORITY,
    "interruptible": True,
    "conditions": [
        global_cmp("Frostfall.esp", "6DCFB", "==", 2),  # FrostfallRunning: 2 = running
        cond("IsInCombat", negated=True),
        multi("OR", player_branch, npc_branch),
    ],
}

mod = {
    "name": "Frostfall Awareness",
    "author": "nosimo",
    "description": "Cold animations driven by Frostfall. Animations from (DMC) Shiver Alpha by distar66 (GPL v3).",
}

if OAR.exists():
    shutil.rmtree(OAR)
count = 0
for gender in ("female", "male"):
    (SUB / gender).mkdir(parents=True, exist_ok=True)
    for hkx in sorted((SRC / gender).glob("*.hkx")):
        shutil.copy2(hkx, SUB / gender / hkx.name.lower())
        count += 1
(OAR / "config.json").write_text(json.dumps(mod, indent=4) + "\n", encoding="utf-8")
(SUB / "config.json").write_text(json.dumps(submod, indent=4) + "\n", encoding="utf-8")
print(f"{count} clips, priority {PRIORITY}")
