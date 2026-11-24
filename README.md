# Echoes of Tyria

A [Nexus](https://raidcore.gg/Nexus) addon for Guild Wars 2 that shows the synced lyrics of the song you are listening to as 3D text placed in the game world, line by line, in time with the music.

Inspired by the Minecraft mod *Show the Lyrics*.

## What it does

- Reads the song playing on your PC through Windows media controls (Spotify, browsers, YouTube Music, and most players that show up in the Windows volume flyout).
- Downloads synced lyrics from [LRCLIB](https://lrclib.net), or uses your own `.lrc` files.
- Places every line as real 3D text in the world: scattered around your view, on walls, tilted toward the camera when you look up or down.
- Hides lyrics behind terrain, buildings and the game's own windows (inventory, chat, map...).
- Korean, Japanese, Chinese and Cyrillic lyrics are drawn with a matching Windows font automatically.

### Options

- 18 appear animations, 8 idle animations and 15 disappear animations
- An effect when each letter lands: flash, ring, sparks or bounce
- 20 bundled fonts plus your own `.ttf` / `.otf` files
- Text, depth, shadow, outline and glow colors, or colors taken from the album cover (the sung color can use the cover's second color)
- 3D depth, shadow, outline, glow
- 17 particle types: sparkles, confetti, embers, bubbles, snow, stars, hearts, music notes, petals, fireflies, glitter, rings, autumn leaves, butterflies, smoke, pixels, rain
- Pulse with the beat of the music
- Karaoke: letters light up while the line is sung
- The chorus is found automatically and stands out (bigger, glowing, more particles)
- Word modes: whole line, words appear inside the line, or each word in its own place
- Repeated words ("yeah, yeah, yeah", "na-na-na") spread out, each in its own spot
- Backing vocals in (brackets) on their own line, in the opposite color
- Optional bad word filter (stars in the middle of the word)
- Song title card with the album cover
- Lyrics placed farther away while on a mount, pinned to the screen, or turning to face you
- Hide during combat, in PvP / WvW, and while typing in chat
- Global and per-song timing offsets, with stretching for slowed / sped up versions
- Playback controls (previous, play / pause, next)

## Install

1. Install [Nexus](https://raidcore.gg/Nexus).
2. In game, open Nexus, go to the addon library and install **Echoes of Tyria**.
   Or download `EchoesOfTyria.dll` from the [releases](https://github.com/3oXYLY/Echoes_of_Tyria/releases) page and put it in `Guild Wars 2/addons`.
3. Play a song, and the lyrics appear around you.

The settings are in the Nexus options, under Echoes of Tyria. Settings, fonts and lyrics live in `Guild Wars 2/addons/EchoesOfTyria`.

## Keybinds

| Action | Default |
|---|---|
| Show / hide lyrics | Alt + Shift + L |
| Previous song | Alt + Shift + 8 |
| Play / pause | Alt + Shift + 9 |
| Next song | Alt + Shift + 0 |
| Lyrics earlier for this song | Alt + Shift + K |
| Lyrics later for this song | Alt + Shift + J |
| Test line | Alt + Shift + F8 |

All of them can be changed in the Nexus keybind settings.

## Your own lyrics

Put `.lrc` files in `Guild Wars 2/addons/EchoesOfTyria/lyrics`, named `Artist - Title.lrc`. A local file is used before LRCLIB.

## Privacy

The addon sends the artist and title of the playing song to LRCLIB to find lyrics. Nothing else leaves your PC.

## Building

Visual Studio 2022 or newer with the C++ desktop workload. Open the folder, pick the `x64 Release` preset and build the `LyricHud` target.

| Target | What it is |
|---|---|
| `LyricHud` | The addon (`EchoesOfTyria.dll`), one DLL with the fonts inside |
| `LyricHudTest` + `LyricHudCore` | Development setup that reloads the core while the game is running |

## Credits

- [Nexus](https://github.com/RaidcoreGG) by Raidcore
- [Dear ImGui](https://github.com/ocornut/imgui), [nlohmann/json](https://github.com/nlohmann/json)
- Lyrics from [LRCLIB](https://lrclib.net)
- Fonts from [Google Fonts](https://fonts.google.com), under the SIL Open Font License or the Apache License 2.0.

This addon was made with the help of AI (Claude).

Licenses of the libraries and fonts included in the addon are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

## License

Copyright (c) 2026 Foxy. All rights reserved.

Echoes of Tyria is free to download and use. The official compiled DLL may be hosted and redistributed unmodified by anyone, including Raidcore and Nexus.

The source code is public so anyone can read it and check what the addon does, but it may not be copied, modified, redistributed or reused in other projects without written permission from the author. See [LICENSE](LICENSE).
