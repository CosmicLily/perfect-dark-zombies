// bo2scan: Black Ops II's own player collision (clip walls, stair ramps,
// barriers, every floor) read out of the running game, for port/src/codmap.c.
// No exporter has it: C2M (Husky) reads the drawn world, the zone dump only
// the entities.
//
// In a Town match, from a few seconds after spawning: the player is made
// invulnerable and ignored, and a grid over Town's box (codmap.c's row box,
// Black Ops II's inches) is traced straight down with playerphysicstrace -
// the player's own collision, clip included - from ztop to zbot, each surface
// it stops on noted and the trace begun again under it. A barrier is a column
// whose surface is far over its neighbours'. Written to
// %LOCALAPPDATA%\Plutonium\storage\t6\scriptdata\bo2scan_<map>.txt:
//   bo2scan 1 <map> <x0> <y0> <step> <ztop> <zbot>
//   <x> <y> <height>...   (each column with any surface, highest first)
// Goes in %LOCALAPPDATA%\Plutonium\storage\t6\scripts\zm\ (README)
#include maps\mp\_utility;
#include common_scripts\utility;

init()
{
	level thread bo2scan_start();
}

bo2scan_start()
{
	level waittill("connected", player);
	player waittill("spawned_player");
	player enableinvulnerability();
	player.ignoreme = 1;
	wait 5;
	player iprintlnbold("bo2scan: scanning the map's collision - leave the game running");
	bo2scan_run(player);
}

bo2scan_run(player)
{
	x0 = -2168;
	x1 = 4785;
	y0 = -4578;
	y1 = 2416;
	step = 16;
	ztop = 1000;
	zbot = -300;
	map = getdvar("mapname");

	f = fs_fopen("bo2scan_" + map + ".txt", "write");
	fs_writeline(f, "bo2scan 2 " + map + " " + x0 + " " + y0 + " " + step + " " + ztop + " " + zbot);

	rows = 0;

	for (x = x0; x <= x1; x += step)
	{
		for (y = y0; y <= y1; y += step)
		{
			line = "";
			z = ztop;
			tries = 0;
			solidtop = 0;
			solidrun = 0;

			while (z > zbot && tries < 48)
			{
				tries++;
				hit = playerphysicstrace((x, y, z), (x, y, zbot - 10));

				// begun inside something solid: a little lower. Solid two steps
				// running (48 inches, more than a floor is thick) is a wall or
				// clip from floor to ceiling, which no downward trace stops on:
				// its top is noted as a surface, so it is walled like one
				if (hit[2] >= z - 1)
				{
					if (solidrun == 0)
					{
						solidtop = z;
					}

					solidrun++;

					if (solidrun == 2)
					{
						line = line + " " + int(solidtop);
					}

					z -= 24;
					continue;
				}

				solidrun = 0;

				// through to the bottom: nothing more under it
				if (hit[2] <= zbot - 5)
				{
					break;
				}

				line = line + " " + int(hit[2]);

				// on under it, past the floor's own thickness
				z = hit[2] - 72;
			}

			if (line != "")
			{
				fs_writeline(f, x + " " + y + line);
			}
		}

		rows++;

		if (rows % 20 == 0)
		{
			player iprintln("bo2scan: " + int(100 * (x - x0) / (x1 - x0)) + "%");
		}

		wait 0.05;
	}

	fs_fclose(f);
	player iprintlnbold("bo2scan: done - the file is in Plutonium's storage\t6\scriptdata");
}
