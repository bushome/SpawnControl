# Limits and churn

[Back to the docs index](README.md)

How to set limits on a map, how zones react to them, and how SpawnControl keeps zones from spawning creatures
it only has to remove again.

- [Why conservative caps](#why-conservative-caps)
- [Surveying a map](#surveying-a-map)
- [Applying a round](#applying-a-round)
- [Churn guard](#churn-guard)
- [Things that look wrong but are not](#things-that-look-wrong-but-are-not)

## Why conservative caps

Every wild creature costs server time, and admin tools that scan a class (creature listers, multitools) keep
the whole list of that class in a fixed buffer. ASE's buffers are smaller than ASA's, so a class with
thousands of wild creatures can overflow them. Conservative caps keep both the tick rate and those tools
healthy.

Starting caps that have worked well:

| Kind | Cap |
|---|---|
| Land and air creatures | 250 |
| Sea life and resource creatures (fish, trilobites, Achatina, dung beetles, Jugbugs, Lantern Goats and similar) | 300 |
| Dragonfly | 125 |

Map-gameplay creatures (corrupted creatures, zombies, the Nameless) are part of a map's challenge; cap them
only when they are clearly out of hand, and usually higher.

## Surveying a map

Survey only once the map is steady: after a restart or a new round of limits, wait until `List` line 3 shows
the removal queue at or near 0, the `Removed in the last 60 s` log line is in single digits and new "held
back" lines have stopped. That is usually 30 to 60 minutes, longer on large ocean maps. A survey taken during
the first burst undercounts.

Then run the game's `ListDinos` command and save its output as `ListDinos.txt` next to
[`tools/Make-Limits.ps1`](../tools/Make-Limits.ps1), and run the script:

```powershell
.\Make-Limits.ps1
```

It writes `limits.txt`: one `"Class_C": cap` line for every class whose wild count is over its cap, largest
first, ready to paste into the map's `Limit` block (the last line has no trailing comma). It also prints a
table of class, wild count and cap to review. Tamed counts are ignored, event `Ghost_` classes are skipped
(`-IncludeGhosts` keeps them), and the caps can be changed with `-LandAirCap`, `-SeaCap` and
`-DragonflyCap`. Which classes count as sea and resource creatures is a list at the top of the script; add a
map's or a mod's own sea creatures there.

If Windows refuses to run it, either run it once with
`powershell -ExecutionPolicy Bypass -File .\Make-Limits.ps1`, or allow local scripts for your account:
`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, then `Unblock-File .\Make-Limits.ps1`.

## Applying a round

1. **Land first, sea second** on a new map. Add sea limits once a `Removed in the last 60 s` line shows the
   map's ocean takes them without churning (see below).
2. Paste the round into the map file, **save it**, then `SpawnControl.Reload` once per map. Batch a map's
   edits into one reload.
3. Wait for the map to settle and survey again.

**Zones refill.** Capping a class does not lower a spawn zone's target count: the zone fills its slots with
its other entries, so the next class grows. Each round shifts less (thousands, then hundreds, then dozens).
Compare the total wild count on the `List` census line before and after a round to see the real effect. Two or three
rounds per map is typical.

**Density vs Limit.** Density removals are not refused at spawn and the churn guard does not cover them, so
a density rule on a creature that respawns quickly churns. Use a `Limit` for those.

**Summoned creatures** are outside every rule. When a class far over its cap shows as summoned in `List`
(for example creatures a mod spawns around another creature), limit the summoner instead. Summons left behind
by a removed summoner have been seen to despawn on their own.

## Churn guard

Zones skip spawn entries whose classes are all full (`RefuseFullEntries`). Some entries cannot be refused:
group entries that mix full and open classes still spawn and their full members are removed, and entries the
plugin cannot read are left to the game. A zone fed by those would replace each removed creature at once,
forever: it is churning.

SpawnControl traces each creature to the zone whose `SpawnNPCs` spawned it and keeps a window of each zone's
last 20 spawns. Once the window is full and at least `ChurnRemoved` (default 12) of those 20 were removed on
arrival, the zone is held back: its spawning stops while every class it churned (the last 4) is still full,
and resumes as soon as one has room. Held zones are kept through `SpawnControl.Reload`, and each re-checks its
classes against the new limits; a server restart starts over.

The log names a zone the first time it is held, with the actual count:
`20 of its last 20 spawns were over a limit and removed on arrival`. `List verbose` shows the zones held now, and the
five unheld zones with the most removals on arrival (`feeding:` lines with their share removed and last-20
count).

Tuning `ChurnRemoved`: lower it if `List verbose` shows unheld zones feeding at a high share; raise it if zones are
held whose other, unlimited classes you want to keep spawning. 12 has needed no tuning so far.

## Things that look wrong but are not

- **A burst of "held back" lines after a restart.** Every zone needs its first 20 spawns before it can be
  held, so they arrive together and taper off.
- **A count slightly over its limit.** Spawns between two census passes are checked against counts that
  include creatures killed since the last pass, so a class can float a little over or under; each pass
  corrects it.
- **A large removal number on the first census after a new round.** That is the overflow being trimmed;
  judge the removal rate once it has drained.
