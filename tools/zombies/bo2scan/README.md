# bo2scan

Reading Black Ops II's own collision (clip walls, stair ramps, barriers) out of
the running game through Plutonium scripts, for port/src/codmap.c. No exporter
here has it: C2M reads the drawn world, the zone dump has only the entities.

Scripts go in `%LOCALAPPDATA%\Plutonium\storage\t6\scripts\zm\`, and run in any
Zombies match. Files they write land in
`%LOCALAPPDATA%\Plutonium\storage\t6\scriptdata\`.

- `bo2scan_test.gsc`: every 2 seconds, the height under the player as the player
  collides (`playerphysicstrace`) and as a bullet does (`bullettrace`).
- `bo2scan_fs.gsc`: writes `bo2scan_test.txt`, if Plutonium's `fs_*` functions are
  there.
