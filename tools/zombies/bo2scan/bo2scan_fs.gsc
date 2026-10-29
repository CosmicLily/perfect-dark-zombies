// bo2scan, test 2: whether a script may write a file (Plutonium's fs_*
// functions). Kept apart from test 1: if these functions are not there, this
// file fails to load and test 1 still runs. The file lands under
// %LOCALAPPDATA%\Plutonium\storage\t6\scriptdata\
init()
{
	level thread bo2scan_fstest();
}

bo2scan_fstest()
{
	wait 8;
	f = fs_fopen("bo2scan_test.txt", "write");
	fs_writeline(f, "bo2scan: file test ok");
	fs_fclose(f);
	iprintlnbold("bo2scan: test file written");
}
