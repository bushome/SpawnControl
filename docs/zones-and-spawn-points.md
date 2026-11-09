# Zones and spawn points

[Back to the docs index](README.md)

*ASA only.* Tools for the opposite problem: places that spawn too little. ASE ignores `EnableZones` and
`SpawnPoints` with a warning and has no `Zones` command.

- [Diagnosing a zone: SpawnControl.Zones](#diagnosing-a-zone-spawncontrolzones)
- [EnableZones](#enablezones)
- [DisableZones](#disablezones)
- [AutoDisableZones](#autodisablezones)
- [SpawnPoints](#spawnpoints)
- [Worked example: a cave that never spawned wyverns](#worked-example-a-cave-that-never-spawned-wyverns)

## Diagnosing a zone: SpawnControl.Zones

```
SpawnControl.Zones [foundations] [text]
```

Console only. Lists the spawn zones whose spawn volumes come within that many foundations of you (default 30,
up to 300), nearest first, at most 12. With `text`, only zones whose name or entry names contain it
(`SpawnControl.Zones 80 wyvern`). Per zone:

- its target (`UseDesiredNumberOfNPC`, with MinDesired, Max and multiplier);
- the game's own counters: total spawned over the server's life and failed spawn placements;
- the wild creatures inside its spawn volumes right now, by class;
- SpawnControl's spawn passes for the zone since load and how many creatures arrived, and whether it is held
  or stopped;
- `DISABLED` if the zone is switched off;
- its spawn volumes, zone volumes, spawn point overrides and placement checks (random point checks, extent,
  overlap and blocking tests, minimum distance from players, structures and tames);
- its entries with weights.

Reading it: many failed placements with few creatures inside point at placement (the creature does not fit
where the zone tries to put it); a small target points at the zone's numbers; `DISABLED` means it never runs
at all. Zones do not spawn near players, so stand outside the spawn area when watching it fill. Also written
to the log. The "largest spawn volume" size is currently not reliable.

## EnableZones

```json
"EnableZones": [ "NPCZoneManager_UAID_..." ]
```

Spawn zones to switch back on, by the object name `SpawnControl.Zones` prints (case does not matter). Some
maps have zones switched off, so they never spawn, whatever the ini spawn entries say. The cause is not always clear;
the state appears to persist in the save. SpawnControl finds them on load and switches them on, then checks again after every census and
switches back on any that something disabled again, logging each time. A name not found on the map is a
warning. `List` shows each one's state.

## DisableZones

```json
"DisableZones": [ "NPCZoneManager_UAID_..." ]
```

The opposite of `EnableZones`: spawn zones to switch off, by the same names, kept off after every census. A
name in both lists is switched off, with a config warning. `List` shows each named zone, its state and the state
SpawnControl keeps it in. A disabled zone makes no spawn passes at all (`Zones` shows `0 spawn passes`).

The case it was built for: water-only zones (`only in water` in `Zones`) whose spawn volume is mostly land.
Each pass runs up to 40 random point checks with traces and fails, about every 14 seconds, forever. On Lost
Colony, five of them around the city plaza had over 3 million failures each and `spawned 0`, and standing among
them gave server FPS dips into the teens; a river with working water zones did not. `List`'s **Failing zones**
section finds these for you (see [Reading List](commands.md#reading-spawncontrollist)) with a location to go
and look. By hand: look for zones with a huge
`spawn failures` count, `spawned 0` and `only in water`, and run `Zones` twice a few minutes apart: if the
failures rise by the number of passes, the zone never succeeds. Switching it off also removes whatever it
would have spawned; put those back with [SpawnPoints](#spawnpoints) if needed.

## AutoDisableZones

```json
"AutoDisableZones": { "Enabled": true, "MinFailedPasses": 500, "MaxZones": 100 }
```

Switches failing zones off by itself, so they don't have to be found and pasted into `DisableZones` by hand.
After each census, a zone is switched off when all of these hold:

- it is failing (see `List`'s **Failing zones**: at most one spawn per 1000 failed passes) and has failed at
  least `MinFailedPasses` spawn passes since load (default 500, about an hour; at least 100);
- SpawnControl never refused one of its entries this load. An entry refused because its creatures are at their
  limit, or a zone stopped by a block area, fails passes too; those zones are never switched off for it;
- it is not a time-of-day zone (`NPCZoneManager_ToD_C`): the game switches those in day/night pairs and spawn
  points can't stand in for them, so they are left to `DisableZones` by hand;
- it is not named in `EnableZones`, `DisableZones` or the auto list already.

Each one is logged (`AutoDisableZones: switched off ...` with its failures, spawns and first three entries) and
added to the `DisableZones` list in `<Map>.auto.json`, next to the auto block areas, so it stays off after a
restart, like a `DisableZones` entry. Delete a name from that file to let the zone run again. Off unless
`Enabled`; the list is still kept off when it is turned off. It needs a map file of its own and a SharedConfig
folder, like [AutoBlockRepeats](under-mesh.md#autoblockrepeats). At most `MaxZones` names are kept per map.

Zone names can change with a game update (the number at the end of the name). A name in the auto list that is no
longer on the map is dropped from the file with a log line, and the zone is switched off again under its new
name once it has failed long enough. Names in `EnableZones` and `DisableZones` are only warned about
(`no spawn zone named ... on this map`); fix those by hand with `SpawnControl.Zones`.

`List` shows the count (`Auto-disabled zones: N in <Map>.auto.json`); `SpawnControl.List failing` lists them.
Switching a zone off also removes whatever it would have spawned; add it back with [SpawnPoints](#spawnpoints)
if it is wanted there.

## SpawnPoints

```json
"SpawnPoints": [ {
  "Name": "Wyvern cave",
  "Classes": { "/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire": 1 },
  "Points": [ [x, y, z] ],
  "Target": 6,
  "AreaRadiusInFoundations": 200,
  "SpawnIntervalSeconds": 120,
  "BaseLevels": [ min, max ]
} ]
```

For places the map's own spawn zones never fill: a cave whose zone cannot fit a large creature, or has no
entry for it. After each census SpawnControl counts the set's classes within `AreaRadiusInFoundations` of the
points' centre (wild and summoned), and while that is under `Target` it spawns one with the game's own wild
spawn: a class picked by weight, at a random one of `Points`.

- At most one spawn per `SpawnIntervalSeconds` (at least 10, default 120). Spawns happen right after a
  census, so the real gap rounds to the census cycle; a quarter of the interval (at most 30 seconds) is
  allowed early so it does not slip a whole census.
- Classes are written as `ListDinos` prints them, with or without the `.Name_C` part.
- Pick points by standing where the creature fits and reading the position (`ccc`). Mid-air points between
  floor and ceiling work well for large flyers.
- Without `BaseLevels` the game rolls the level as for any wild spawn; with it, the base level is drawn from
  that range.
- What it spawns counts as summoned, so it is outside the other rules.

The log names each spawn with its level, and each point where the game could not place the creature; `List`
shows each set's count against its target and how each point is doing.

## Worked example: a cave that never spawned wyverns

A cave meant for wyverns spawned one or two at best, even after ini spawn edits.

1. `SpawnControl.Zones` from inside the cave showed its zone `DISABLED`, with zero creatures inside although
   its lifetime counter showed tens of thousands of spawns.
2. Adding the zone to `EnableZones` brought the cave's ground creatures back within 15 minutes.
3. `SpawnControl.Zones 300 wyvern` showed no zone near the cave had a wyvern entry at all, so no ini edit
   could have worked.
4. A `SpawnPoints` set with the four wyvern classes, six mid-air points and a target of 18 filled the cave
   within a few hours.
