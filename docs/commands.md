# Commands

[Back to the docs index](README.md)

Admin console commands (`cheat SpawnControl.List`). `List`, `Rules` and `Reload` also work over RCON; the
others need a player position and are console only. Every command's output is also written to the log.

| Command | Side | What it does |
|---|---|---|
| `SpawnControl.List [text]` | both | The plugin's status. See below. With text: every class whose path contains it, with counts, rules, full path and parent classes (for `Exclude` and `Protect.Families`). |
| `SpawnControl.Rules` | both | What this server is running with: the section and why it was picked, every file read, the General settings, then one line per map value saying whether it is the section's own or from `Default`, e.g. `UnderMesh.ChecksPerSecond = 40  (map; default 20)` or `Limit.Dodo_Character_BP_C removed  (map; default 250)`. |
| `SpawnControl.Reload` | both | Re-reads `config.json` (and the shared file or folder it points to). A broken file keeps the old settings and reports the error. Live counts carry over, so limits keep holding without waiting for a census: classes that already had a rule keep their count, newly ruled classes start from the last census. |
| `SpawnControl.MeshHere [foundations]` | both | Runs the game's under-mesh test right now on every creature within that many foundations of you (default 30, up to 200), whatever its state, and lists each one, under first, then nearest first: class, distance, height difference, wild/mission/summoned/tamed, awake or in stasis, the result (`UNDER (overlapping)`, `UNDER (below)` or `clear`) with what the test hit, and the hit and ground heights. For checking suspects and finding misses. See [Under-mesh](under-mesh.md#reading-suspects). |
| `SpawnControl.Zones [foundations] [text]` | ASA | Spawn zones near you, for zones that spawn too little. See [Zones and spawn points](zones-and-spawn-points.md#diagnosing-a-zone-spawncontrolzones). |
| `SpawnControl.Links <text>` | ASA | For wild creatures whose class path contains the text: what they are linked to (owner, instigator, wild following parent, followers they lead, zone manager link), as counts. One pass over all actors; for diagnosing creatures that spawn others. |

## Reading `SpawnControl.List`

The first lines are the quick health check:

- **Map and census:** the map name the server reports, the config section in use and why, and the last
  census (how long ago, how long it took, actors seen) with the total wild count, mission-owned and summoned
  creatures counted separately.
- **Queue:** the removal queue and how many have been removed (`Queue N, removed N`). A queue at or near 0
  means the map is steady.

Then, in order: rolling refresh total; spawn-check timing and per-tick time spent on census slices and
removals (average and worst, in ms); zone entries checked and refused; zones held back for churning and the
unheld zones feeding the most removals; every limited or blocked class and every group with its count now and
each member's count; the density rules with the 10 fullest cubes each found last census, one per spot (count,
location for `setplayerpos`, top classes); under-mesh results, repeat spots, stopped zones and block areas
(auto areas marked `(auto)`); on ASA, spawn points, failing zones, auto-disabled zones (count; `List failing`
lists them), the zones named in `EnableZones` and `DisableZones`, and the Genesis ocean band; the report
families; and the ten largest classes without a rule, which are the candidates for a limit.

**Failing zones** (ASA) lists zones that keep trying and almost never spawn: at least 100 failed spawn
passes since load (the game counts one failure per failed pass; zones pass every ~7 s, so about 12 minutes)
and at most one spawn per 1000 failures, worst first (10 shown; `SpawnControl.List failing` shows all), each
with its failures, spawns, spawn passes, `only in water` if set, its first three spawn entries and where its
first spawn volume is. Every failed pass costs point checks and traces again, so
these are FPS drains worth switching off with `DisableZones`, or automatically with `AutoDisableZones`; see
[Zones and spawn points](zones-and-spawn-points.md#disablezones). `SpawnControl.List failing` also lists the
zones `AutoDisableZones` has switched off. The section only appears when a zone
qualifies, and counts start at load, so give the map a while after a restart.

Counts are the last census minus what it queued for removal, plus spawns since. Max values are kept since
load.

**Checking for hitches without a player online:** `List` over RCON shows the worst tick the census and the
removals have taken. A few milliseconds is invisible; if the removals' max climbs, lower `RemovalsPerTick`,
and if the census slice's does, lower `CensusActorsPerTick`.
