OpenCE (Halo: Combat Evolved) as an installed title
====================================================

1. Put in this folder:
   - hacbrewpack.exe   (https://github.com/The-4n/hacBrewPack/releases)
   - prod.keys         (your console's, made by Lockpick_RCM: SD card switch\prod.keys)
2. Double-click pack-title.bat. It takes the maps from ..\maps (the folder
   beside this one), or run it as:  pack-title.bat "C:\path\to\maps"
3. Install hacbrewpack_nsp\0100484C4F430000.nsp with DBI or Tinfoil
   (needs sigpatches, as for any homebrew title).
4. Start "Halo: Combat Evolved" from the Home menu.

The settings (config.toml), saves and logs stay on the SD card in
switch\opence. The maps are read from inside the title, so the SD card's
switch\opence\maps folder can be deleted after installing (a map there is
used instead of the title's, if one is left).

When a new OpenCE build comes, it comes with a new kit: pack and install
again (installing over the old title keeps its settings and saves).
