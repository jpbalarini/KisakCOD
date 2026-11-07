# Bots (Bot Warfare)

AI players for the headless dedicated server (`serve.py --dedicated`, or `dedicated.js --bots=web/bots`). The AI is
[Bot Warfare for CoD4](https://github.com/ineedbots/iw3_bot_warfare) by INeedGames, as GSC scripts; the engine side (the
built-in functions it expects from a CoD4x server) is `web/platform/bots_web.cpp`.

**The server starts with no bots.** You add them yourself, from the server console (the terminal running `serve.py`):

| command | what it does |
|---|---|
| `addbot [count]` | adds that many bots (default 1). They take player slots (`sv_maxclients`) and come back on every new map (the engine drops them at a map change, the scripts re-add the same number) |
| `kickbots` | removes every bot and forgets how many there were |
| `kick <name>` | removes one (bots are named bota, botb, ... from `scriptdata/botnames.txt`) |
| `set bots_manage_fill 8` | keep the server filled up to 8 players, humans included: bots are added when there are fewer (with `set bots_manage_fill_kick 1` they also leave again as humans join; `bots_manage_fill_mode` and `_spec` change what is counted) |

The server itself can also be told at start-up: `--dedicated-args "+set bots_manage_fill 6"`, or `set bots_manage_fill 6` in
`web/dedicated/server.cfg`. `--dedicated-bots ''` (serve.py) turns all bot support off.

Useful dvars (all upstream's, set them in the console or the config; the full list is in upstream's README):

| dvar | default | |
|---|---|---|
| `bots_skill` | 0 | 0 mixed levels, 1 (easy) to 7 (hard), 8 per team (`bots_skill_axis_hard`, ...), 9 completely random |
| `bots_team` | autoassign | `allies`, `axis`, `autoassign`, `custom` (`bots_team_amount` bots on axis); `bots_team_force 1` keeps them there |
| `bots_main_chat` | 1.0 | how often bots chat; **0 = silent** |
| `bots_play_move`, `_fire`, `_nade`, `_knife`, `_obj`, `_camp`, `_aim`, `_ads` | 1 | switch single behaviours off (e.g. `bots_play_fire 0` = harmless sparring partners) |
| `bots_manage_add` | 0 | what `addbot` sets; the scripts add that many and reset it |

Differences from running upstream on CoD4x (`maps/mp/bots/_kisak.gsc` is the only file that is not upstream's):

- `bots_main_firstIsHost` is 0 here (upstream: the first player to connect gets host powers); the in-game bot menu and the waypoint
  editor are not started.
- Movement uses `botmovement(forward, right)` (the usercmd's own axes) rather than CoD4x's `botmoveto(origin)`; `botmeleeparams` works
  (CoD4x removed the lunge, this engine keeps it).
- Waypoints are loaded for the 15 stock maps only (`scriptdata/waypoints/mp_*_wp.csv`). On a map without a file the bots stand around
  (`maps/mp/bots/waypoints/_custom_map.gsc` is where a map's waypoints would come from).
- Not in the browser: a tab that *hosts* a game (listen server) has no bot support; the scripts are read from the host's disk, which a
  browser cannot.
- Stock `scr_testclients N` (with `developer_script 1`) still adds the engine's old random-input test clients; those do not use any of this.

## Credits and licence

Everything below `maps/mp/bots/` except `_kisak.gsc`, plus `scriptdata/`, is from
<https://github.com/ineedbots/iw3_bot_warfare> (commit `901895a`, 2025-05-21), unmodified. Upstream has no licence file; its README says:
"Feel free to use code, host on other sites, host on servers, mod it and merge mods with it, just give credit where credit is due."
Its credits: CoD4x Team, INeedGames, PeZBot team, Ability, Salvation. The waypoints are the work of Freddy & Metaljag, CannerZ45,
JAK (BF2all), MikeTheGuy, Botmancod, Hugo HG and Stendby (see upstream's `scriptdata/waypoints/README.md`).
The CoD4x server ([callofduty4x/CoD4x_Server](https://github.com/callofduty4x/CoD4x_Server), AGPL-3.0) was read for what its bot
built-ins do; no code was copied from it.
