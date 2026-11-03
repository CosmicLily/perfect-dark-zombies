# Perfect Zombies

A Call of Duty style **Zombies** mode for the Perfect Dark PC port, made to play
like Black Ops II's: rounds, boarded windows, the Mystery Box, perk machines,
Pack-a-Punch, wall buys and power-ups. Pick it as the scenario in the Combat
Simulator.

**[Download the beta](https://github.com/CosmicLily/perfect-dark-zombies/releases/tag/dabs-mod-dev)**
— Windows, Linux and macOS. It is a work in progress, rebuilt with every fix;
**Check for Updates** in the game fetches the newest one.

## You need a ROM

The game reads everything out of a Perfect Dark ROM and ships with none of it.
You need `Perfect Dark (USA) (Rev 1)` (`ntsc-final`, md5
`e03b088b6ac9e0080440efed07c1e40f`), dumped from your own cartridge.

1. Unpack the download somewhere.
2. Put the ROM in the `data` folder next to the game, named **`pd.ntsc-final.z64`**.
3. Run `pd.x86_64.exe` on Windows, `./pd.x86_64` on Linux, or `./pd.x86_64` /
   `./pd.arm64` on macOS.

## Credits and licence

- Built on [Dab's Mod](https://github.com/DabDavis/perfect-dark-dabs-mod) by DabDavis.
- The PC port: [perfect-dark-pc-port/perfect_dark](https://github.com/perfect-dark-pc-port/perfect_dark) and contributors
- The decompilation: [n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark), Ryan Dwyer and contributors
- Perfect Dark is © Rare / Microsoft. Call of Duty and Black Ops II are © Activision.
  This project ships no game assets and is not affiliated with any of them.

MIT, same as the port and the decompilation it came from — see `LICENSE`.
[CREDITS.md](CREDITS.md) has the full account.

- RAR archives are read with RARLAB's UnRAR source (`port/src/external/unrar`),
  © Alexander Roshal, under its own licence: UnRAR source code may be used in
  any software to handle RAR archives without limitations free of charge, but
  cannot be used to develop RAR (WinRAR) compatible archiver and to re-create
  RAR compression algorithm, which is proprietary. Distribution of modified
  UnRAR source code in separate form or as a part of other software is
  permitted, provided that full text of this paragraph, starting from "UnRAR
  source code" words, is included in license, or in documentation if license
  is not available, and in source code comments of resulting package. The
  full text is in `license.txt` beside the source.
- 7z archives are read with the LZMA SDK (`port/src/external/lzma`), public
  domain, Igor Pavlov.
