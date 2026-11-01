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
3. **Zones skip full classes.** `ANPCZoneManager::IsValidNPCEntry` is hooked. When a zone weighs one of its
   spawn entries and every creature class in it is blocked or at its limit (or its group is), the entry is
   refused and the zone picks another one, so it spawns other species instead of creatures that would only
   be removed. The spawn check above stays as the backstop for anything that still gets through.
4. **Removal.** Nothing is removed in the hook or in bulk. The queue drains `RemovalsPerTick` creatures per
   second. Each one is checked again first (still alive, still wild, not a mission creature, its class still
   has a rule) and then destroyed: no corpse, no loot, no death effects. A creature within
   `NearPlayerDistance` of a player goes to the back of the queue, up to 30 times, so removals happen out of
   sight.
5. **Rolling refresh.** With `Refresh.TurnoverHours` set, each census also queues its share of a slow
   turnover of the whole wild population: enough that every creature is replaced in about that many hours
   (24 h on 20,000 creatures is about 14 a minute), oldest first. The zones respawn them with freshly rolled
   levels, so a periodic full dino wipe is no longer needed. Ages count from server start, so right after
   a restart every creature is equally old until `MinAgeMinutes` have passed. A pick is dropped, not
   deferred, if by its turn it is within `PlayerDistance` of a player, took damage within
   `RecentDamageSeconds`, is unconscious, being tamed or young; the next census picks another.

Which creatures are left alone:

- **Owned:** any creature on a player or tribe team (team 50000 and up), so all tames.
- **Mission:** any creature whose `OwnerMission` is set. This covers outpost garrisons and raids on Lost
  Colony and missions on Genesis maps, even when they use the same classes as ordinary wild creatures.
- **Protected:** classes whose path contains a `Protect.PathContains` entry (`/Missions/` by default) or that
  derive from a `Protect.Families` class.

A reload of `config.json` re-resolves every class and clears the queue. Live counts carry over, so limits keep
holding; blocked classes and density rules are applied again by the next census.

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

`//` and `/* */` comments are allowed. A comma right before a closing `]` or `}` is ignored with a warning naming its line, so a list trimmed to its last entry still loads; a missing comma still fails the load. Every key the plugin does not know is reported (`Config: Maps.LostColony.Overlapp is not a known setting here; ignored`), and a rule placed one level off says where it belongs, so a misplaced block is not silently ignored.

Comments are allowed. `configs\config.commented.json` has the same values with every setting explained.

### One config for every server: SharedConfig

Instead of a copy per server, the plugin's own `config.json` can hold a single line pointing at a shared file
or folder:

```json
{ "SharedConfig": "D:\\servers\\shared\\SpawnControl" }
```

A relative path is taken from the plugin's folder. Every other key in that file is then ignored (with a
warning). The path can be:

- **a file**: a whole config in the usual layout, read by every server that points at it.
- **a folder**: `config.json` for `General`, `Protect`, `Refresh` and `ReportFamilies` (and optionally
  `Default` and `Maps`), `Default.json` for the `Default` section, and one `<section>.json` per map holding
  only that map's rules, e.g. `Aberration.json` containing `{ "Limit": { ... }, "UnderMesh": { ... } }`. The
  file name is the section name, matched as `Maps` sections are. A map file replaces a section of the same
  name in the folder's `config.json` (with a warning). Other files are ignored.

If the shared file or folder cannot be read, or any file in it is not valid JSON, the load fails: at startup
nothing is ruled until it is fixed, and a reload keeps the rules already in force. The load line in the log,
`SpawnControl.Reload` and `SpawnControl.Rules` name every file read, so a server that fell back to its own copy
shows it. Without `SharedConfig` the plugin reads its own `config.json` as before.

### Default

A top-level `Default` section holds rules every map starts from; it takes the same keys as a map section.
Each map section is laid over it: an object merges key by key (so a map's `Limit` adds to and overrides the
default limits, and `"UnderMesh": { "ChecksPerSecond": 40 }` changes only that value), a list or a single
value replaces the default one (a map's `Exclude` list replaces the default list, it does not add to it), and
`null` deletes a default entry (`"Limit": { "Dodo_Character_BP_C": null }`). A map with no section of its own
runs the `Default` rules as they are; without `Default` it runs none, as before. A problem in `Default` is
reported once, as `Default.<key>`, not again for every map. `SpawnControl.Rules` shows the result on each server.

### General

| Setting | Default | Meaning |
|---|---|---|
| `RemovalsPerTick` | 10 | Most creatures removed per second. |
| `CensusActorsPerTick` | 5000 | Level actors the census looks at per second. |
| `CensusIntervalSeconds` | 60 | Seconds between census passes. |
| `NearPlayerDistance` | 10000 | Unreal units (300 = one foundation). Queued creatures closer than this to a player wait. |
| `RefuseFullEntries` | true | Let zones skip spawn entries whose creatures are all blocked or at their limit. An entry whose class the plugin cannot read (not loaded through that entry) is left to the game; `List` counts those. A zone whose spawns are still mostly over a limit (`ChurnRemoved` of its last 20 removed on arrival) is churning: it would replace each removed creature at once, forever. Its spawning is held back while the classes it churned are full and resumes as soon as one has room; the log names it the first time, `List` shows the zones held now and the five unheld zones with the most removals on arrival. Held zones are kept through `SpawnControl.Reload`; each re-checks its classes against the new limits. |
| `ChurnRemoved` | 12 | How many of a zone's last 20 spawns must be removed on arrival before it is held back (1 to 20). Group entries that mix full and open classes still spawn, and their full members are removed, so a churning zone often sits well below 20 of 20. Lower it if `List` shows unheld zones with a high share removed; raise it if zones are held that you want to keep spawning. |
| `IgnoreSummoned` | true | Leave creatures summoned by another creature (a Gloon's Revenant escorts) out of every rule: a wild creature that appears after the first census without a spawn zone spawning it. They are counted in the census and shown in `List`. Limit the summoner instead. |
| `LogRemovals` | true | Once a minute, log how many creatures were removed and how many zone spawn entries were refused, by class. Once a class is at its limit, this shows how hard the zones still push for it. Also the under-mesh lines: up to 20 a minute name each removal (or, in report mode, each new suspect), and one line a minute counts the rest. With it off none of these are logged; `List` shows the same counts and spots either way. |

### Protect

| Setting | Default | Meaning |
|---|---|---|
| `PathContains` | `["/Missions/"]` | Classes whose path contains any of these are never touched. |
| `Families` | `[]` | Classes deriving from any of these are never touched, e.g. `Thrall_Character_BP_Base_C`. |

### Refresh

| Setting | Default | Meaning |
|---|---|---|
| `TurnoverHours` | 0 in code, 24 shipped | The wild population turns over in about this many hours. 0 = off. |
| `MinAgeMinutes` | 60 | Never refresh a creature first seen less than this long ago. |
| `PlayerDistance` | 30000 | Never refresh a creature this close to a player (300 m). |
| `RecentDamageSeconds` | 300 | Never refresh a creature that took damage this recently. |
| `SkipPathContains` | `["/Missions/", "Thrall", "Boss"]` | Classes whose path contains any of these are never refreshed. |

Tamed, mission-owned and protected creatures are never refreshed. Refreshes are logged once a minute
(`Refreshed in the last 60 s`) and counted in `SpawnControl.List`.

### ReportFamilies

Label -> parent class. Each becomes a summary row in `SpawnControl.List` (wild without a mission, and
mission-owned). Reporting only. Shipped: `"Thralls": "Thrall_Character_BP_Base_C"`.

### Maps

One section per map, picked by the map name the server reports (case-insensitive; ASA's `_WP` suffix may be
left off). `-SpawnControlMap=<section>` on the server command line picks a section regardless of map, for a
test server that should use another map's rules. Without it, a section named after the `-ServerKey=` value
on the command line, or `?ServerKey=` in the map URL (any case, as the other plugins read it), wins over the map name when one exists, so sections can
be keyed the same way across plugins. `SpawnControl.List` and the "Map …: using section …" log line say
which one was picked and why.

| Setting | Meaning |
|---|---|
| `Block` | Classes whose wild creatures are removed as soon as they appear. |
| `Limit` | Class -> maximum wild count on this map. `0` means no limit. |
| `GroupLimit` | Name -> `{ "Max": n, "Classes": [...] }`. One maximum shared by all the classes listed. When over, creatures are taken from whichever class has the most, so both shrink together. |
| `Density` | Key -> `{ "Max": n, "AreaInFoundations": a }`, optionally with `"Classes": [...]` and `"Exclude": [...]` (classes the rule never counts, such as packs meant to be dense; a short name here also matches every class derived from it, so `"Thrall_Character_BP_Base_C"` keeps out every thrall; an entry starting with `/` matches every class whose path contains it, so `"/ZombieDino/"` keeps out a whole folder). Clump cap: no cube `a` foundations across may hold more than `n` of the class (the key), of the listed classes together, or of every wild creature (key `"*"`). Checked each census on two grids, the second shifted by half a cube, so a clump on a cube edge is still caught. Over-full spots lose their creatures farthest from players first; removals still wait for players to move off as usual. Area is capped at 200 foundations. |
| `DensitySkip` | `{ "Caves": true, "Areas": [ { "Name": "City", "Location": [x, y, z], "RadiusInFoundations": r } ] }`. Where density rules do not apply: creatures the game spawned in a cave or boss area (default on), and horizontal circles of `r` foundations around a point (z is ignored; `ccc` in game gives x and y). Use it for places that are meant to be dense, such as a city, corrupted zones or a boss arena. Blocks, limits and groups still apply there. |
| `BlockAreas` | `[ { "Name": "Spawner", "Location": [x, y, z], "RadiusInFoundations": r } ]`. Places where no wild creature belongs, such as a misplaced spawner or the space under the map. Every wild creature inside is removed whatever its class (tames, mission and protected creatures excepted), by the census and as soon as it spawns. A zone that puts 3 creatures inside and none anywhere else has all its spawn entries refused from then on, so it stops spawning there; the log names it and `List` shows it. With `[x, y, z]` the area is a sphere, so the ground above an underground spot is untouched; with `[x, y]` it is a column at any height. A reload clears what was learned about zones. The fullest-cube line under a density rule in `List` is a quick way to find such spots. |
| `AutoBlockRepeats` | `{ "Enabled": true, "MinRemoved": 5, "MaxRadiusInFoundations": 15, "MaxAreas": 50 }`, off unless `Enabled`. After each census, a spot where at least `MinRemoved` under-mesh removals happened within 10 foundations (the repeat spots `List` shows) becomes a block area by itself, named `Auto N <class>`, as long as it is at most `MaxRadiusInFoundations` wide (wider spots are too scattered to be one spawn point and stay in `List`). The log names each one. It then works like a pasted area: creatures inside are removed and zones that only spawn there are stopped. A map keeps at most `MaxAreas`. Needs `UnderMesh` with `Remove`, a SharedConfig folder and a map file for the map: the areas are saved to `<Map>.auto.json` next to it, in the BlockAreas format, and read back on every load (also with `Enabled` false). Edit or delete that file by hand to change them, then reload; to keep one for good, move it into the map file. Only that map's server writes it. `List` marks auto areas `(auto)`. |
| `Overlap` | `{ "Fraction": 0.5, "Exclude": [...] }`. Creatures spawned inside one another: two overlap when their centres are closer than `Fraction` (0.1 to 1) of their summed collision radii, read from each creature's capsule. Of each stack, the one nearest a player stays and the rest are removed, before limits are counted. Riders, carried creatures, summoned, mission and protected creatures are never counted; `Exclude` works as for `Density`. Small flyers that pass through other creatures (Dragonfly) can be caught mid-pass; add them to `Exclude`. Leave the key out to turn it off. `List` shows how many the last census found and where. |
| `UnderMesh` | `{ "ChecksPerSecond": 20, "Remove": false, "RemoveNoGround": false, "IncludeStasis": false, "StopZones": true, "TraceGround": false, "IgnoreFoliage": true, "Exclude": [...] }`. Reported always; removed only with `"Remove": true`. Creatures under the map or inside its geometry, found with the game's own under-mesh test (`UVictoryCore::IsUnderMesh`, the check that lifts a player who ghosts under the map). Each census queues the wild creatures; the timer tests them in a shuffled order at `ChecksPerSecond` (up to 1000), each creature once per census. By default only awake creatures (near players) are tested; with `IncludeStasis` creatures in stasis are tested too, which finds buried or broken spawners anywhere on the map at about 20 µs a test (a 21,000-creature map needs roughly 200 a second to cover it every census). Creatures the game flagged as spawned in a cave or in a block area are skipped. Places the mapmaker marked as allowed under the surface (caves and the like) are skipped. `List` shows how many were tested, the cost per test and the last 10 suspects with location; the log names each new one (up to 20 a minute, while `LogRemovals` is on and `Remove` is off). With `Remove`, a creature is queued for removal only when the test finds it below the surface (not only overlapping, which a creature clipping a rock edge can show for a moment) on two separate censuses; a clear result in between starts over. Wild followers of a removed creature (young, pack members) are removed with it. Caves tested clear on Lost Colony. `List` also groups the last 1,500 removals into repeat spots (3 or more within 10 foundations, not already in a block area) and prints each with a `BlockAreas` line to paste: a spot like that is a spawn point inside the terrain, and blocking it stops its zone instead of feeding the removal queue. `"StopZones": true` (the default; acts only with `Remove`) does the same without a block area: each creature is traced to the zone that spawned it, and a zone with 5 or more of its creatures removed under the mesh and at most one creature testing clear per 5 removed is stopped, as a block area stops one. That catches zones spread over a wide patch of bad ground, or behind a cave wall, where a block area would also cover the real ground or cave next to it; a zone that also spawns where creatures belong keeps enough clear creatures never to be stopped. Only creatures spawned after the first census are traced, so it starts counting as the zones respawn. The log names each stopped zone; `List` lists them with their counts. A reload starts over. Each suspect line also gives where the test hit relative to the creature (`hit 4.1 fdn above`); with `"TraceGround": true` it also traces straight down from the creature and gives the distance to the first surface (`ground 0.3 fdn below`, or `no ground within 100 fdn below`). Both are reported only. `"RemoveNoGround": true` (without `Remove`) removes only creatures found below on two censuses with no ground within 100 foundations under them, creatures that have fallen out of the world, and leaves the rest reported only; it suits maps whose ceilings, overhangs and stacked areas make the plain test unreliable. With `"IgnoreFoliage": true` (the default) a creature whose test hits foliage (a tree trunk, a bush: a `Foliage` component) is traced for ground, and counted clear when ground is below it: it stands on the ground under a plant. With no ground below it counts as under as before. `List` counts them; `MeshHere` marks them `[foliage with ground below: counted clear]`. Repeat spots in `List` come from removals with `Remove`, and otherwise from suspects (each creature counted once), with a summary of their ground readings, so a map in report mode still shows where to check and place block areas. |
| `AsaGen1OceanBand` | ASA only, `GenesisSimulation` section only (its own map file, not `Default`): `{ "Remove": false }`. Genesis 1 in ASA has a large ocean whose floor lies flat at about Z 255,200, with the other biomes stacked far below it (highest top, volcanic, at Z 76,843). The gap between them is empty: a wild creature there has fallen through the ocean floor or spawned under it. Inside the ocean's X/Y (-1,064,334 to 1,165,061 by -1,113,644 to 1,115,730), each census finds wild creatures with Z between 80,000 and 254,250, or 182,450 over the deeper coral zone (about -128,036 to 126,996 by -202,655 to 203,305). Reported only by default: `List` gives the count at the last census, the top classes and the last 10 creatures found with their location; the log names each new one (up to 20 a minute, while `LogRemovals` is on). With `"Remove": true` they are removed like creatures in a block area, whatever their class. Protected classes, tamed, mission and summoned creatures are left out. Anywhere else the key is ignored with a warning. |

Class keys are either a short class name (`Otter_Character_BP_C`), which matches every class of that name
from any mod, or a full path exactly as `ListDinos` prints it
(`/Game/PrimalEarth/Dinos/Otter/Otter_Character_BP.Otter_Character_BP_C`), which matches one class. Matching
is exact, never by parent class, so `Zombie_Character_BP_Skinny_C` does not catch
`Zombie_Character_BP_Skinny_Outpost_C`. For `Limit`, a full-path entry wins over a short-name entry for the
same class. A class can have a `Limit` and be in groups; every rule it is in must hold. Density rules run after all the others, on what they leave.

## Commands

Admin console commands (`cheat SpawnControl.List`); both also work over RCON.

| Command | What it does |
|---|---|
| `SpawnControl.List [text]` | Map, config section, last census, queue, removals, rolling refresh total, spawn-check timing, per-tick time spent on census slices and removals (average and worst, in ms), and zone entries checked and refused; zones held back for churning and the unheld zones feeding the most removals; then every limited or blocked class and every group with its count now and each member's count, the density rules with the 10 fullest cubes each found last census, one per spot (count, location for `setplayerpos`, top classes) (the last census minus what it queued for removal, plus spawns since), the report families and the ten largest classes without a rule. With text: every class whose path contains it, with counts, rules, full path and parent classes (for `Exclude` and `Protect.Families`). Also written to the log. |
| `SpawnControl.Rules` | What this server is running with: the section and why it was picked, every file read, the General settings, then one line per map value saying whether it is the section's own or from `Default`, e.g. `UnderMesh.ChecksPerSecond = 40  (map; default 20)` or `Limit.Dodo_Character_BP_C removed  (map; default 250)`. Also written to the log. |
| `SpawnControl.Reload` | Re-reads `config.json` (and the shared file or folder it points to). A broken file keeps the old settings and reports the error. Live counts carry over, so limits keep holding without waiting for a census: classes that already had a rule keep their count, newly ruled classes start from the last census. |
| `SpawnControl.MeshHere [foundations]` | Console only. Runs the game's under-mesh test right now on every creature within that many foundations of you (default 30, up to 200), whatever its state, prints how many came back clear and under, and lists each one, under first, then nearest first: class, distance, height difference, wild/mission/summoned/tamed, awake or in stasis, and the result (`UNDER (overlapping)`, `UNDER (below)` or `clear`) with what the test hit. For checking suspects and finding misses. Also written to the log. Each line also gives the hit and ground heights described under `UnderMesh`; the ground trace always runs here. |
| `SpawnControl.Zones [foundations]` | Console only. Lists the spawn zones whose spawn or zone volumes come within that many foundations of you (default 30, up to 300), nearest first, for zones that spawn too little: live count against the zone's target (`NumNPCSpawned` of `UseDesiredNumberOfNPC`, with MinDesired, Max and multiplier), the game's own count of failed spawn placements, SpawnControl's spawn passes for the zone since load and how many creatures arrived (and whether it is held or stopped), its spawn volumes (nearest, largest), spawn point overrides, placement checks (random point checks, extent, overlap and blocking tests, minimum distance from players, structures and tames), and its spawn entries with weights and classes. Many failures with few arrivals point at placement; a small target points at the zone's numbers. Zones do not spawn near players, so stand outside the spawn area when watching it fill. Also written to the log. |
| `SpawnControl.Links <text>` | For wild creatures whose class path contains the text: what they are linked to (owner, instigator, wild following parent, followers they lead, zone manager link), as counts. One pass over all actors; for diagnosing creatures that spawn others. Also written to the log. |

## Troubleshooting

- **`Config not loaded: ...` at startup.** The message names the problem. Until it is fixed and
  `SpawnControl.Reload` is run, nothing is limited.
- **`section none` in `List`.** No `Maps` section matches the map name shown on the same line.
- **`Could not hook ANPCZoneManager.IsValidNPCEntry...`.** The AsaApi on the server does not know that function.
  Limits still hold; zones just keep spawning full classes for the queue to remove, so the per-minute
  `Removed` line stays busier.
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
