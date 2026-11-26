# Under-mesh and block areas

[Back to the docs index](README.md)

Creatures can end up under the map or inside its geometry: spawn points buried in terrain, a seabed spawner
sitting slightly below the floor, creatures that fall through the world. SpawnControl finds them with the
game's own test, removes the ones it is sure about, and helps you stop the spawn points that keep producing
them.

- [UnderMesh](#undermesh): the test and its settings
- [Reading suspects](#reading-suspects): what the hit and ground readings mean
- [Repeat spots and stopped zones](#repeat-spots-and-stopped-zones)
- [BlockAreas](#blockareas): places where no wild creature belongs
- [AutoBlockRepeats](#autoblockrepeats): block areas made automatically
- [AsaGen1OceanBand](#asagen1oceanband): Genesis 1's empty layer (ASA only)
- [Lessons from live maps](#lessons-from-live-maps)

## UnderMesh

```json
"UnderMesh": {
  "ChecksPerSecond": 20, "Remove": false, "RemoveNoGround": false, "IncludeStasis": false,
  "StopZones": true, "TraceGround": false, "IgnoreFoliage": true, "Exclude": [ ... ]
}
```

The test is the game's own `UVictoryCore::IsUnderMesh`, the check that lifts a player who ghosts under the
map. SpawnControl only takes its answer.

| Setting | Default | Meaning |
|---|---|---|
| `ChecksPerSecond` | 20 | Tests per second (up to 1000). Each census queues the wild creatures and the timer tests them in a shuffled order, each creature once per census. A test costs about 20 µs; a 21,000-creature map needs roughly 200 a second to cover it every census. |
| `IncludeStasis` | false | Also test creatures in stasis, not only awake ones near players. This finds buried or broken spawners anywhere on the map. |
| `Remove` | false | Remove creatures found below the surface on two separate censuses. Without it, suspects are only reported. |
| `RemoveNoGround` | false | Without `Remove`: remove only creatures found below on two censuses with no ground within 100 foundations under them (fallen out of the world); report the rest. Suits maps whose ceilings, overhangs and stacked areas make the plain test unreliable. |
| `StopZones` | true | With `Remove`: stop zones that spawn mostly under the mesh. See [below](#repeat-spots-and-stopped-zones). |
| `TraceGround` | false | Trace straight down from each suspect and report the distance to the first surface. |
| `IgnoreFoliage` | true | A creature whose test hits foliage is traced for ground and counted clear when ground is below it. |
| `Exclude` | `[]` | Classes never tested; works as for `Density` (short names match derived classes, `/` entries match paths). Use it for burrowers and creatures that live inside rock (deathworms, basilisks, Nameless). |

What is skipped: tames, mission and class-protected creatures, excluded classes; creatures the game flagged as spawned in a
cave or in a block area; and, on ASA, places the mapmaker marked as allowed under the surface (caves and the
like). The ASE API has no such exclusion zones, so on ASE check suspects in caves before trusting them.
`Protect.Buffs` does not skip this test or confirmed mesh removals, including inside block areas that buff
protection otherwise exempts. All other mesh exclusions and confirmation requirements still apply.

**Removal is conservative.** A creature is queued only when the test finds it *below* the surface (not only
overlapping, which a creature clipping a rock edge can show for a moment) on two separate censuses; a clear
result in between starts over. On ASA, wild followers of a removed creature (young, pack members) are removed
with it; buff-protected followers require their own mesh confirmation. The ASE API has no follower list,
so there they are tested on their own.

**Foliage.** A creature standing under a tree or bush can test as under, because the hit is the plant. With
`IgnoreFoliage`, a hit on a `Foliage` component is traced for ground: with ground below the creature counts
clear; with none it counts as under as before. `List verbose` counts these and `MeshHere` marks them
`[foliage with ground below: counted clear]`.

**Reporting.** `List verbose` shows how many were tested, the cost per test and the last 10 suspects with location.
With `LogRemovals` on and `Remove` off, the log names each new suspect (up to 20 a minute).

## Reading suspects

Each suspect line gives where the test hit relative to the creature (`hit 4.1 fdn above`). With
`TraceGround` it also gives the distance to the first surface below (`ground 0.3 fdn below`, or
`no ground within 100 fdn below`). `SpawnControl.MeshHere` always traces ground.

- **No ground within 100 foundations:** the creature is out of the world, or embedded so deep the trace
  starts inside rock. Removing it is right.
- **Ground just below:** not a false positive by itself. Creatures embedded in rock still stand on terrain.
  Use repeat spots and block areas.
- **A tight cluster at one Z value per species:** creatures standing on a floor (a city level, a cave
  floor, a ledge). **A spread of Z values:** terrain, or creatures inside rock.

## Repeat spots and stopped zones

`List verbose` groups the last 1,500 removals into repeat spots: 3 or more within 10 foundations, not already in a
block area. Each is printed with a `BlockAreas` line to paste. A spot like that is a spawn point inside the
terrain, and blocking it stops its zone instead of feeding the removal queue. Without `Remove`, repeat spots
come from suspects instead (each creature counted once), with a summary of their ground readings, so a map
in report mode still shows where to look.

`StopZones` (the default, acting only with `Remove`) does the same without a block area. Each creature is
traced to the zone that spawned it, and a zone with 5 or more of its creatures removed under the mesh and at
most one creature testing clear per 5 removed is stopped, as a block area stops one. That catches zones
spread over a wide patch of bad ground, or behind a cave wall, where a block area would also cover the real
ground or cave next to it. A zone that also spawns where creatures belong keeps enough clear creatures never
to be stopped. Only creatures spawned after the first census are traced, so it starts counting as the zones
respawn. The log names each stopped zone and `List verbose` lists them; a reload starts over.

## BlockAreas

```json
"BlockAreas": [ { "Name": "Spawner", "Location": [x, y, z], "RadiusInFoundations": r } ]
```

Places where no wild creature belongs, such as a misplaced spawner or the space under the map. Every wild
creature inside is removed whatever its class (tames, mission and protected creatures excepted), by the
census and as soon as it spawns. With `[x, y, z]` the area is a sphere, so the ground above an underground
spot is untouched; with `[x, y]` it is a column at any height.

A zone that puts 3 creatures inside and none anywhere else has all its spawn entries refused from then on, so
it stops spawning there; the log names it and `List verbose` shows it. A reload clears what was learned about zones.

Block areas are tied to the map's geometry, so ASA areas do not carry over to ASE. The fullest-cube line
under a density rule in `List verbose` is another quick way to find spots worth blocking.

## AutoBlockRepeats

```json
"AutoBlockRepeats": { "Enabled": true, "MinRemoved": 5, "MaxRadiusInFoundations": 15, "MaxAreas": 50 }
```

Off unless `Enabled`. After each census, a spot where at least `MinRemoved` under-mesh removals happened
within 10 foundations becomes a block area by itself, named `Auto N <class>`, as long as it is at most
`MaxRadiusInFoundations` wide (wider spots are too scattered to be one spawn point and stay in `List verbose`). The
log names each one. It then works like a pasted area: creatures inside are removed and zones that only spawn
there are stopped. A map keeps at most `MaxAreas`.

It needs `UnderMesh` with `Remove`, a [SharedConfig folder](config-reference.md#one-config-for-every-server-sharedconfig)
and a map file for the map. The areas are saved to `<Map>.auto.json` next to it, in the `BlockAreas` format,
and read back on every load (also with `Enabled` false). Edit or delete that file by hand to change them,
then reload; to keep one for good, move it into the map file. Only that map's server writes it. `List verbose` marks
auto areas `(auto)`.

## AsaGen1OceanBand

*ASA only, `GenesisSimulation` section only* (its own map file, not `Default`):

```json
"AsaGen1OceanBand": { "Remove": false }
```

Genesis 1 in ASA has a large ocean whose floor lies flat at about Z 255,200, with the other biomes stacked
far below it (the highest top, volcanic, at Z 76,843). The gap between them is empty: a wild creature there
has fallen through the ocean floor or spawned under it. Inside the ocean's X/Y (-1,064,334 to 1,165,061 by
-1,113,644 to 1,115,730), each census finds wild creatures with Z between 80,000 and 254,250, or 182,450 over
the deeper coral zone (about -128,036 to 126,996 by -202,655 to 203,305).

Reported only by default: `List verbose` gives the count at the last census, the top classes and the last 10
creatures found with their location; the log names each new one (up to 20 a minute, while `LogRemovals` is
on). With `"Remove": true` they are removed like creatures in a block area, whatever their class. Protected
classes, tamed, mission and summoned creatures are left out. Anywhere else the key is ignored with a warning.
The check is a few comparisons per creature during the census, so leaving it on costs nothing.

## Lessons from live maps

- The game's test is unreliable wherever there is geometry overhead: stacked biomes, reef ledges, overhangs,
  biome seams, multi-level cities. On such maps start with `RemoveNoGround` or report mode and use block
  areas for the rest.
- Repeat spots checked in game have so far always been real, cave spots included; no cave exception has been
  needed.
- Excluding a class is a last resort. Prefer `IgnoreFoliage` for creatures that climb trees, and block areas
  for floors that test as under.
- `IsPointStuckWithinMesh` lines in the server log are the game's own stuck check (teleports, unstuck), not
  SpawnControl.
