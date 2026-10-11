# cookies (Plugify)

Per-player persistent cookies (name/value strings keyed by SteamID64) stored in MySQL or SQLite. A port of the Metamod:Source plugin 'cookies' by Pisex; the table layout is kept compatible so both variants can share one database.

Plugify port (fork) of **cookies (Client Cookies)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-client_cookies
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk

## Installation
Download `cookies-1.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/cookies`.

## License
GPL-3.0, see LICENSE.

