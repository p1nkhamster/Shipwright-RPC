# Discord Rich Presence

Shows what you're doing in the game on your Discord profile, and updates as you
move between the menus, gameplay and cutscenes.

<p align="center">
  <img src="https://i.imgur.gg/986jZRd-link_house_rando.png" alt="Exploring Link's House, Young Link | Randomizer" width="300">
  <img src="https://i.imgur.gg/gTz32wH-cutscene.png" alt="Watching a Cutscene, Young Link" width="300">
</p>

## What it shows

The top line is the current activity:

| Situation             | Top line                     |
| --------------------- | ---------------------------- |
| Title screen / menus  | `In the Menus`               |
| Exploring an area     | `Exploring Kakariko Village` |
| In a dungeon          | `Exploring Water Temple`     |
| A cutscene is playing | `Watching a Cutscene`        |

The bottom line is always Link's age (`Young Link` or `Adult Link`). If you're
playing Randomizer or Boss Rush, that's added after it, e.g.
`Young Link | Randomizer`.

Area names come straight from the game's scene table, so they're always right
without keeping a list in the code. Discord adds the elapsed timer on its own.

## Turning it on and off

It's off by default. Turn it on under Settings > General > Discord Rich
Presence, or from the console:

```
set gEnhancements.DiscordRPC 0   # off
set gEnhancements.DiscordRPC 1   # on
```

Turning it off clears the presence right away and turning it back on reconnects.

## Requirements

You need the Discord desktop app running and logged in. The web version won't
work, since it doesn't expose the local socket the game connects to. If Discord
isn't running the game carries on as normal and tries again later.

This is desktop only (Windows, Linux, macOS). The Switch and Wii U builds have
no Discord client, so the whole thing is compiled out there and costs nothing.

## How it works

Everything lives in
[soh/soh/Enhancements/DiscordRPC/DiscordRPC.cpp](../soh/soh/Enhancements/DiscordRPC/DiscordRPC.cpp).
It talks to Discord over its local IPC socket (a named pipe on Windows, a Unix
socket on Linux and macOS) and builds the messages with `nlohmann/json`, which
the project already uses, so there's no extra dependency or Discord SDK to pull
in.

It runs off the `OnGameStateMainStart` hook every frame but only sends an update
when the displayed text changes. Where each value comes from:

| Shown value         | Source                                                          |
| ------------------- | --------------------------------------------------------------- |
| In gameplay vs menu | `gPlayState != NULL && gSaveContext.gameMode == GAMEMODE_NORMAL` |
| Cutscene playing    | `gPlayState->csCtx.state != CS_STATE_IDLE`                      |
| Area name           | `SohUtils::GetSceneName(gPlayState->sceneNum)`                  |
| Link's age          | `LINK_IS_CHILD`                                                 |
| Randomizer          | `IS_RANDO`                                                      |
| Boss Rush           | `IS_BOSS_RUSH`                                                  |

## Using your own Discord application

The name and icon come from a Discord application. This build points at app ID
`1511502474530263141`. To use your own:

1. Make an app at https://discord.com/developers/applications
2. Under Rich Presence > Art Assets, upload a square image (at least 512x512) and
   set its key to `logo`.
3. Put your application ID in `DISCORD_APP_ID` at the top of `DiscordRPC.cpp`.

New art assets can take a few minutes before the icon shows up.

## Notes

Cutscenes aren't named individually, just shown as "Watching a Cutscene". OoT
doesn't store readable cutscene names, only a scene and cutscene index, so
there's nothing useful to put there. Link's age is shown as text instead of an
icon to avoid needing more uploaded art.
