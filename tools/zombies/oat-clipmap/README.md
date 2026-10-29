# oat-clipmap

Black Ops II's map collision (clipMap) exported exactly, for port/src/codmap.c's
`readClipMap()`. OpenAssetTools loads T6 clip maps but has no dumper for them;
`ClipMapDumperT6.{h,cpp}` is one.

1. Clone github.com/Laupetin/OpenAssetTools (`--recursive`), copy the two files to
   `src/ObjWriting/Game/T6/Maps/`, and in `src/ObjWriting/Game/T6/ObjWriterT6.cpp`
   include `Maps/ClipMapDumperT6.h` and register
   `RegisterAssetDumper(std::make_unique<clip_map::DumperT6>());` where the
   commented `AssetDumperClipMap` line is.
2. Run `generate.bat` (fetches premake), then build the whole solution:
   `MSBuild build/OpenAssetTools.slnx -p:Configuration=Release -p:Platform=x64`
   (building UnlinkerCli alone skips the generated sources).
3. `export_town_collision.bat` (in the OpenAssetTools folder) writes
   `clipdump/zm_transit/maps/mp/zm_transit.d3dbsp.clip.txt`. The asset type is
   `clipmap` (OAT's ASSET_TYPE_CLIPMAP_PVS; `clipmap_unused` is the other).

The converter reads it from there by default (or `--codmap-clip <file>`).
