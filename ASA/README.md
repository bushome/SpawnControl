# SpawnControl (ASA)

An [AsaApi](https://github.com/ArkServerApi/AsaApi) plugin for ARK: Survival Ascended servers. It keeps wild
creature populations in check per map: blocked classes, a maximum per class, and shared maximums for groups
of classes. Tamed, player-owned and mission or outpost creatures are never touched.

The ARK: Survival Evolved build lives in `..\ASE`.

## How it works

1. **Census.** Every second the plugin looks at `CensusActorsPerTick` level actors. When it has been through
   all of them (a few seconds on a busy map) it has a count of every wild creature by class, and it plans
   which creatures to remove so every rule holds: blocked classes first, then, per rule, the creatures
   farthest from any player. A new pass starts every `CensusIntervalSeconds`.
2. **Spawn check.** `APrimalDinoCharacter::BeginPlay` is hooked. Once the first census has finished, a wild
   creature that appears over its limit, over its group's limit, or of a blocked class is queued right away.
   The check is a cached rule lookup and a counter comparison; `SpawnControl.List` shows its average and
   worst time.
3. **Removal.** Nothing is removed in the hook or in bulk. The queue drains `RemovalsPerTick` creatures per
   second. Each one is checked again first (still alive, still wild, not a mission creature, its class still
   has a rule) and then destroyed: no corpse, no loot, no death effects. A creature within
   `NearPlayerDistance` of a player goes to the back of the queue, up to 30 times, so removals happen out of
   sight.

Which creatures are left alone:

- **Owned:** any creature on a player or tribe team (team 50000 and up), so all tames.
- **Mission:** any creature whose `OwnerMission` is set. This covers outpost garrisons and raids on Lost
  Colony and missions on Genesis maps, even when they use the same classes as ordinary wild creatures.
- **Protected:** classes whose path contains a `Protect.PathContains` entry (`/Missions/` by default) or that
  derive from a `Protect.Families` class.

A reload of `config.json` re-resolves every class and clears the queue; limits wait for the next census to
have real counts again, blocked classes are queued again by that census.

## Build

Requirements: Visual Studio 2026 with the "Desktop development with C++" workload (v145 toolset), and Git.

AsaApi is a git submodule at `extern\AsaApi`. After cloning the repository:

```
git submodule update --init --recursive
```

1. Build `extern\AsaApi\AsaApi.sln` (**Release / x64**) so `AsaApi.lib` lands in `extern\AsaApi\out_lib\`,
   or copy `AsaApi.lib` there from the `Lib` folder of the AsaApi release zip your servers run.
2. Open `SpawnControl.sln`, select **Release / x64**, build.

`fmt` comes from vcpkg through the manifest in `vcpkg.json` (Visual Studio's bundled vcpkg; triplet
`x64-windows-static-md`). AsaApi's `UnrealString.h` includes `fmt/format.h`, so it has to be on the include
path.

The result is `out\SpawnControl\`, containing `SpawnControl.dll`, its `.pdb`, `PluginInfo.json`, `config.json`,
`config.commented.json` and `THIRD-PARTY-NOTICES.md`. Keep the notice file with the DLL when redistributing
the plugin.

## Install

The server machine needs a Microsoft Visual C++ x64 Redistributable at least as new as the Visual Studio
the plugin was built with.

1. If another mod or plugin limits or blocks spawns on the same map, take it out first, or both will act on
   the same creatures.
2. Copy the `out\SpawnControl` folder to `ShooterGame\Binaries\Win64\ArkApi\Plugins\`. The folder and the DLL
   must keep the same name.
3. Start the server. `ArkApi.log` shows `Config loaded: ... using <section> for map <name>`. About a minute
   after the world is up the first census finishes; if anything is over its rules, the log shows
   `Census: ... N over the rules (...), M newly queued, queue Q`, and once a minute
   `Removed in the last 60 s: total (per class, largest first)` while removals happen.

To update a running server, drop the new build in as `SpawnControl.dll.arkapi`. If automatic plugin
reloading is enabled in AsaApi's own `config.json`, AsaApi swaps it in.

## First run on each map

1. Run `SpawnControl.List`. The first line shows the map name the server reports and which config section is
   in use. If it says `section none`, add a section under `Maps` with that name (or the name without `_WP`)
   and run `SpawnControl.Reload`.
2. The list ends with the ten largest classes that have no rule: the candidates for a limit.
3. `SpawnControl.List <text>` shows every class whose path contains the text, with wild and mission counts
   and the full path to paste into the config, e.g. `SpawnControl.List otter`.

## config.json

Comments are allowed. `configs\config.commented.json` has the same values with every setting explained.

### General

| Setting | Default | Meaning |
|---|---|---|
| `RemovalsPerTick` | 10 | Most creatures removed per second. |
| `CensusActorsPerTick` | 5000 | Level actors the census looks at per second. |
| `CensusIntervalSeconds` | 60 | Seconds between census passes. |
| `NearPlayerDistance` | 10000 | Unreal units (300 = one foundation). Queued creatures closer than this to a player wait. |
| `LogRemovals` | true | Once a minute, log how many creatures were removed and of which classes. Once a class is at its limit, this shows how much the zones keep refilling it. |

### Protect

| Setting | Default | Meaning |
|---|---|---|
| `PathContains` | `["/Missions/"]` | Classes whose path contains any of these are never touched. |
| `Families` | `[]` | Classes deriving from any of these are never touched, e.g. `Thrall_Character_BP_Base_C`. |

### ReportFamilies

Label -> parent class. Each becomes a summary row in `SpawnControl.List` (wild without a mission, and
mission-owned). Reporting only. Shipped: `"Thralls": "Thrall_Character_BP_Base_C"`.

### Maps

One section per map, picked by the map name the server reports (case-insensitive; ASA's `_WP` suffix may be
left off). `-SpawnControlMap=<section>` on the server command line picks a section regardless of map, for a
test server that should use another map's rules.

| Setting | Meaning |
|---|---|
| `Block` | Classes whose wild creatures are removed as soon as they appear. |
| `Limit` | Class -> maximum wild count on this map. `0` means no limit. |
| `GroupLimit` | Name -> `{ "Max": n, "Classes": [...] }`. One maximum shared by all the classes listed. When over, creatures are taken from whichever class has the most, so both shrink together. |

Class keys are either a short class name (`Otter_Character_BP_C`), which matches every class of that name
from any mod, or a full path exactly as `ListDinos` prints it
(`/Game/PrimalEarth/Dinos/Otter/Otter_Character_BP.Otter_Character_BP_C`), which matches one class. Matching
is exact, never by parent class, so `Zombie_Character_BP_Skinny_C` does not catch
`Zombie_Character_BP_Skinny_Outpost_C`. For `Limit`, a full-path entry wins over a short-name entry for the
same class. A class can have a `Limit` and be in groups; every rule it is in must hold.

## Commands

Admin console commands (`cheat SpawnControl.List`); both also work over RCON.

| Command | What it does |
|---|---|
| `SpawnControl.List [text]` | Map, config section, last census, queue, removals, spawn-check timing and per-tick time spent on census slices and removals (average and worst, in ms); then every limited or blocked class with its count, every group, the report families and the ten largest classes without a rule. With text: every class whose path contains it, with counts, rules and full path. Also written to the log. |
| `SpawnControl.Reload` | Re-reads `config.json`. A broken file keeps the old settings and reports the error. |

## Troubleshooting

- **`Config not loaded: ...` at startup.** The message names the problem. Until it is fixed and
  `SpawnControl.Reload` is run, nothing is limited.
- **`section none` in `List`.** No `Maps` section matches the map name shown on the same line.
- **`Could not hook APrimalDinoCharacter.BeginPlay()...`.** The AsaApi on the server does not know that
  function, usually because it is out of date for the game version. The census still enforces every rule,
  only less promptly.
- **Checking for hitches without a player online.** `SpawnControl.List` over RCON shows the worst tick the
  census and the removals have taken. A few milliseconds is invisible; if the removals' max climbs, lower
  `RemovalsPerTick`, and if the census slice's does, lower `CensusActorsPerTick`.
- **A count sits slightly over its limit.** Spawns between two census passes are checked against counts that
  include creatures killed since the last pass, so a class can float a little over or under its limit; each
  pass corrects it.

## Tests

`tests\logic_tests.cpp` covers config parsing, map section lookup, class rules and the removal plan. It needs
no game and no Windows. From a Visual Studio developer prompt in this folder:

```
cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp
logic_tests.exe configs\config.json --shipped
logic_tests.exe configs\config.commented.json --shipped
```

Passing a config file validates it and prints any warnings, a quick way to check an edit before putting it
on a server. `--shipped` also checks it against the shipped values.

## Files

| File | Purpose |
|---|---|
| `src\Main.cpp` | Census, spawn hook, removal queue, commands. |
| `src\SpawnLogic.h` | Config parsing, class rules, removal plan. No game dependencies. Same file in `..\ASE`. |
| `configs\config.json` | Default configuration with the Lost Colony section. |
| `configs\config.commented.json` | The same configuration with every setting explained. Not read by the plugin. |
| `configs\PluginInfo.json` | Plugin metadata read by AsaApi. |
| `tests\logic_tests.cpp` | Stand-alone tests for `SpawnLogic.h` and the shipped config. |
| `vcpkg.json` | vcpkg manifest (fmt). |
| `extern\AsaApi` | AsaApi submodule the plugin builds against. |
