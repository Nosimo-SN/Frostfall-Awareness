# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nosimo-SN. Part of Frostfall Awareness.
"""Builds the Frostfall Awareness FOMOD zip.

Usage: python -I package.py <project root> <version>
Writes build/dist/Frostfall_Awareness-<version>.zip:
  fomod/info.xml, fomod/ModuleConfig.xml
  core/SKSE/Plugins/...          (DLL, licence files, SkyrimNet plugin)   - required
  cold_shiver/meshes/...         (OAR cold animations from Shiver Alpha) - optional
"""
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
version = sys.argv[2]
dist = root / "build" / "dist"
dist.mkdir(parents=True, exist_ok=True)
out = dist / f"Frostfall_Awareness-{version}.zip"

INFO = f"""<?xml version="1.0" encoding="UTF-8"?>
<fomod>
  <Name>Frostfall Awareness</Name>
  <Author>nosimo</Author>
  <Version>{version}</Version>
  <Description>SkyrimNet plugin: the people around you notice Frostfall's cold, warm you, and remember rescuing you.</Description>
</fomod>
"""

MODULE = """<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>Frostfall Awareness</moduleName>
  <requiredInstallFiles>
    <folder source="core" destination="" />
  </requiredInstallFiles>
  <installSteps order="Explicit">
    <installStep name="Options">
      <optionalFileGroups order="Explicit">
        <group name="Cold Animations" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Cold Shiver (OAR)">
              <description>Shivering idle and walk animations while cold, through Open Animation Replacer (required for this option). The player shivers from Frostfall's Very Cold on; NPCs outdoors shiver while you are at least Chilly and the air is cold enough for what they wear (revealing clothing is read from Advanced Nudity Detection, if installed). Vampires, the undead and children never shiver. No game plugin.

Animations from (DMC) Shiver Alpha by distar66, GPL v3. If another mod already gives you cold animations (shivering idles or cold walks, Shiver Alpha included), use either those or this option, not both, so they don't compete.

Skip this if you are near your animation limit.</description>
              <files>
                <folder source="cold_shiver" destination="" />
              </files>
              <typeDescriptor>
                <type name="Optional" />
              </typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>
"""

SHIVER_NOTICE = """Cold Shiver animations
======================

The .hkx animations in this option come unchanged from "(DMC) Shiver Alpha - A Frostfall and Frostbite
Animation Mod" by distar66 (Nexus Mods, Skyrim Special Edition mod 34304), released under the GNU General
Public License v3. They are redistributed here under the same licence; the full text is in
Frostfall_Awareness_LICENSE.txt. The Open Animation Replacer conditions (config.json) are by nosimo, also
GPL v3.

Thank you, distar66.
"""

core_files = {
    "SKSE/Plugins/Frostfall_Awareness.dll": root / "build/Frostfall_Awareness.dll",
    "SKSE/Plugins/Frostfall_Awareness_LICENSE.txt": root / "LICENSE",
    "SKSE/Plugins/Frostfall_Awareness_THIRD_PARTY_NOTICES.md": root / "THIRD_PARTY_NOTICES.md",
}
plugin_dir = root / "skyrimnet/nosimo.frostfall_awareness"
for f in plugin_dir.rglob("*"):
    if f.is_file():
        core_files["SKSE/Plugins/SkyrimNet/external/nosimo.frostfall_awareness/" + f.relative_to(plugin_dir).as_posix()] = f

anim_root = root / "animations"
anim_files = {f.relative_to(anim_root).as_posix(): f for f in anim_root.rglob("*") if f.is_file()}

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("fomod/info.xml", INFO)
    z.writestr("fomod/ModuleConfig.xml", MODULE)
    for arc, src in sorted(core_files.items()):
        z.write(src, "core/" + arc)
    for arc, src in sorted(anim_files.items()):
        z.write(src, "cold_shiver/" + arc)
    anim_oar = "cold_shiver/meshes/actors/character/animations/OpenAnimationReplacer/Frostfall Awareness/"
    z.writestr(anim_oar + "Cold_Shiver_CREDITS.txt", SHIVER_NOTICE)
    z.write(root / "LICENSE", anim_oar + "Frostfall_Awareness_LICENSE.txt")

print(f"{out}: {len(core_files)} core files, {len(anim_files) + 2} animation files")
