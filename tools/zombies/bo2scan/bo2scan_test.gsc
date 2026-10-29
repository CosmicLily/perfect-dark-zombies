// bo2scan, test 1: whether a script sees Black Ops II's player collision.
// In a match, a few seconds after spawning, the floor under the player is
// traced twice - as the player moves (playerphysicstrace, clip and stair
// ramps included) and as a bullet flies (bullettrace, the drawn world) - and
// both heights shown. On a staircase the two differ by the ramp. Goes in
// %LOCALAPPDATA%\Plutonium\storage\t6\scripts\zm\ (tools/zombies/bo2scan/README)
#include maps\mp\_utility;
#include common_scripts\utility;

init()
{
	level thread bo2scan_onconnect();
}

bo2scan_onconnect()
{
	for (;;)
	{
		level waittill("connected", player);
		player thread bo2scan_onspawn();
	}
}

bo2scan_onspawn()
{
	self endon("disconnect");
	self waittill("spawned_player");
	wait 3;

	for (;;)
	{
		start = self.origin + (0, 0, 60);
		end = self.origin - (0, 0, 1000);
		p = playerphysicstrace(start, end);
		t = bullettrace(start, end, false, self);
		self iprintln("bo2scan: at " + int(self.origin[0]) + " " + int(self.origin[1]) + " physics " + int(p[2]) + " bullet " + int(t["position"][2]));
		wait 2;
	}
}
