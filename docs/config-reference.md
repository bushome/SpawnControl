# Config reference

[Back to the docs index](README.md)

The plugin reads `ArkApi/Plugins/SpawnControl/config.json`. `//` and `/* */` comments are allowed, and
`configs/config.commented.json` in each build folder has the shipped values with every setting explained.
A comma right before a closing `]` or `}` is ignored with a warning naming its line, so a list trimmed to its
last entry still loads; a missing comma still fails the load. Every key the plugin does not know is reported
(`Config: Maps.LostColony.Overlapp is not a known setting here; ignored`), and a rule placed one level off
says where it belongs, so a misplaced block is not silently ignored.

Settings marked *ASA only* are ignored with a warning on ASE; see [ASA and ASE differences](asa-vs-ase.md).

## Contents

- [One config for every server: SharedConfig](#one-config-for-every-server-sharedconfig)
- [Default](#default)
- [General](#general)
- [Protect](#protect)
- [Refresh](#refresh)
- [ReportFamilies](#reportfamilies)
- [Maps](#maps): how a section is picked, and every per-map rule
- [Class keys](#class-keys)

## One config for every server: SharedConfig

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
  name in the folder's `config.json` (with a warning). `<section>.auto.json` files are written by
  [AutoBlockRepeats](under-mesh.md#autoblockrepeats) and attached to their map, not read as sections. Other
  files are ignored.

If the shared file or folder cannot be read, or any file in it is not valid JSON, the load fails: at startup
nothing is ruled until it is fixed, and a reload keeps the rules already in force. The load line in the log,
`SpawnControl.Reload` and `SpawnControl.Rules` name every file read, so a server that fell back to its own copy
shows it. Without `SharedConfig` the plugin reads its own `config.json`.

## Default

A top-level `Default` section (or `Default.json` in a shared folder) holds rules every map starts from; it
takes the same keys as a map section. Each map section is laid over it:

- an object merges key by key, so a map's `Limit` adds to and overrides the default limits, and
  `"UnderMesh": { "ChecksPerSecond": 40 }` changes only that value;
- a list or a single value replaces the default one, so a map's `Exclude` list replaces the default list
  rather than adding to it (repeat the default entries you want to keep);
- `null` deletes a default entry: `"Limit": { "Dodo_Character_BP_C": null }`.

A map with no section of its own runs the `Default` rules as they are; without `Default` it runs none. A
problem in `Default` is reported once, as `Default.<key>`, not again for every map. Within one file, a key
written twice takes the later value. `SpawnControl.Rules` shows the merged result on each server.

## General

| Setting | Default | Meaning |
|---|---|---|
| `RemovalsPerTick` | 10 | Most creatures removed per second. |
| `CensusActorsPerTick` | 5000 | Level actors the census looks at per second. |
| `CensusIntervalSeconds` | 60 | Seconds between census passes. |
| `NearPlayerDistance` | 10000 | Unreal units (300 = one foundation). Queued creatures closer than this to a player wait. |
| `RefuseFullEntries` | true | Let zones skip spawn entries whose creatures are all blocked or at their limit, and hold back zones that churn. See [Limits and churn](limits-and-churn.md#churn-guard). |
| `ChurnRemoved` | 12 | How many of a zone's last 20 spawns must be removed on arrival before it is held back (1 to 20). See [Limits and churn](limits-and-churn.md#churn-guard). |
| `IgnoreSummoned` | true | Leave creatures summoned by another creature out of every rule: a wild creature that appears after the first census without a spawn zone spawning it. They are counted in the census and shown in `List`. Limit the summoner instead. |
| `LogRemovals` | true | Once a minute, log how many creatures were removed and how many zone spawn entries were refused, by class. Once a class is at its limit, this shows how hard the zones still push for it. Also the under-mesh lines: up to 20 a minute name each removal (or, in report mode, each new suspect), and one line a minute counts the rest. With it off none of these are logged; `List` shows the same counts and spots either way. |

## Protect

| Setting | Default | Meaning |
|---|---|---|
| `PathContains` | `["/Missions/"]` | Classes whose path contains any of these are never touched. |
| `Families` | `[]` | Classes deriving from any of these are never touched, e.g. `Thrall_Character_BP_Base_C`. |

## Refresh

| Setting | Default | Meaning |
|---|---|---|
| `TurnoverHours` | 0 in code, 24 shipped | The wild population turns over in about this many hours. 0 = off. |
| `MinAgeMinutes` | 60 | Never refresh a creature first seen less than this long ago. |
| `PlayerDistance` | 30000 | Never refresh a creature this close to a player (300 m). |
| `RecentDamageSeconds` | 300 | Never refresh a creature that took damage this recently. |
| `SkipPathContains` | `["/Missions/", "Thrall", "Boss"]` | Classes whose path contains any of these are never refreshed. |

Tamed, mission-owned and protected creatures are never refreshed. Refreshes are logged once a minute
(`Refreshed in the last 60 s`) and counted in `SpawnControl.List`. How the turnover works is in
[How it works](how-it-works.md#the-cycle).

## ReportFamilies

Label -> parent class. Each becomes a summary row in `SpawnControl.List` (wild without a mission, and
mission-owned). Reporting only. Shipped: `"Thralls": "Thrall_Character_BP_Base_C"`.

## Maps

### How a section is picked

One section per map, picked in this order:

1. `-SpawnControlMap=<section>` on the server command line, regardless of map. Useful for a test server
   that should run another map's rules.
2. A section named after the `-ServerKey=` value on the command line, or `?ServerKey=` in the map URL (any
   case, as the other plugins read it), when one exists. Keying sections this way works the same on ASA and
   ASE whatever the map names are.
3. The map name the server reports (case-insensitive; on ASA the `_WP` suffix may be left off).
4. `Default`, if there is one.

The log's `Map ...: using section ...` line, `SpawnControl.List` and `SpawnControl.Rules` say which section was
picked and why.

### Per-map rules

| Setting | Meaning |
|---|---|
| `Block` | Classes whose wild creatures are removed as soon as they appear. |
| `Limit` | Class -> maximum wild count on this map. `0` means no limit. |
| `GroupLimit` | Name -> `{ "Max": n, "Classes": [...] }`. One maximum shared by all the classes listed. When over, creatures are taken from whichever class has the most, so both shrink together. |
| `Density` | Key -> `{ "Max": n, "AreaInFoundations": a }`, optionally with `"Classes": [...]` and `"Exclude": [...]`. Clump cap: no cube `a` foundations across may hold more than `n` of the class (the key), of the listed classes together, or of every wild creature (key `"*"`). `Exclude` lists classes the rule never counts, such as packs meant to be dense: a short name also matches every class derived from it (`"Thrall_Character_BP_Base_C"` keeps out every thrall), and an entry starting with `/` matches every class whose path contains it (`"/ZombieDino/"` keeps out a whole folder). Checked each census on two grids, the second shifted by half a cube, so a clump on a cube edge is still caught. Over-full spots lose their creatures farthest from players first; removals still wait for players to move off as usual. Area is capped at 200 foundations. Density removals are not refused at spawn, so a density rule on a creature that respawns quickly churns; use a `Limit` for those. |
| `DensitySkip` | `{ "Caves": true, "Areas": [ { "Name": "City", "Location": [x, y, z], "RadiusInFoundations": r } ] }`. Where density rules do not apply: creatures the game spawned in a cave or boss area (default on; ASA only, the ASE API has no cave flag), and horizontal circles of `r` foundations around a point (z is ignored; `ccc` in game gives x and y). Use it for places that are meant to be dense, such as a city, corrupted zones or a boss arena. Blocks, limits and groups still apply there. |
| `Overlap` | `{ "Fraction": 0.5, "Exclude": [...] }`. Creatures spawned inside one another: two overlap when their centres are closer than `Fraction` (0.1 to 1) of their summed collision radii, read from each creature's capsule. Of each stack, the one nearest a player stays and the rest are removed, before limits are counted. Riders, carried creatures, summoned, mission and protected creatures are never counted; `Exclude` works as for `Density`. Small flyers that pass through other creatures (Dragonfly) can be caught mid-pass; add them to `Exclude`. Leave the key out to turn it off. `List` shows how many the last census found and where. |
| `UnderMesh` | Find and remove creatures under the map or inside its geometry. See [Under-mesh and block areas](under-mesh.md#undermesh). |
| `BlockAreas` | Places where no wild creature belongs. See [Under-mesh and block areas](under-mesh.md#blockareas). |
| `AutoBlockRepeats` | Turn repeat under-mesh spots into block areas automatically. See [Under-mesh and block areas](under-mesh.md#autoblockrepeats). |
| `AsaGen1OceanBand` | *ASA only*, Genesis 1 only. See [Under-mesh and block areas](under-mesh.md#asagen1oceanband). |
| `EnableZones` | *ASA only*. Switch disabled spawn zones back on. See [Zones and spawn points](zones-and-spawn-points.md#enablezones). |
| `SpawnPoints` | *ASA only*. Spawn creatures where the map's zones never do. See [Zones and spawn points](zones-and-spawn-points.md#spawnpoints). |

## Class keys

Class keys are either a short class name (`Otter_Character_BP_C`), which matches every class of that name
from any mod, or a full path exactly as `ListDinos` prints it
(`/Game/PrimalEarth/Dinos/Otter/Otter_Character_BP.Otter_Character_BP_C`), which matches one class. Matching
is exact, never by parent class, so `Zombie_Character_BP_Skinny_C` does not catch
`Zombie_Character_BP_Skinny_Outpost_C`. For `Limit`, a full-path entry wins over a short-name entry for the
same class. A class can have a `Limit` and be in groups; every rule it is in must hold. Density rules run
after all the others, on what they leave.

`SpawnControl.List <text>` prints the full path and parent classes of every class whose path contains the
text, ready to paste.
