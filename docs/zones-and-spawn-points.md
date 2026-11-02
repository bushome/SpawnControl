# Zones and spawn points

[Back to the docs index](README.md)

*ASA only.* Tools for the opposite problem: places that spawn too little. ASE ignores `EnableZones` and
`SpawnPoints` with a warning and has no `Zones` command.

- [Diagnosing a zone: SpawnControl.Zones](#diagnosing-a-zone-spawncontrolzones)
- [EnableZones](#enablezones)
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
