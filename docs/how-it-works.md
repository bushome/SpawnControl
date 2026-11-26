# How SpawnControl works

[Back to the docs index](README.md)

SpawnControl keeps wild creature populations in check, per map: classes that never spawn, a maximum count
per class, shared maximums for groups of classes, clump caps, and removal of creatures stuck under or inside
the map. Only wild creatures are touched.

## The cycle

1. **Census.** Every second the plugin looks at `CensusActorsPerTick` level actors. When it has been through
   all of them (a few seconds on a busy map) it has a count of every wild creature by class, and it plans
   which creatures to remove so every rule holds: blocked classes first, then, per rule, the creatures
   farthest from any player. A new pass starts every `CensusIntervalSeconds`.
2. **Spawn check.** `APrimalDinoCharacter::BeginPlay` is hooked. Once the first census has finished, a wild
   creature that appears over its limit, over its group's limit, or of a blocked class is queued right away.
   The check is a cached rule lookup and a counter comparison; `SpawnControl.List verbose` shows its average and
   worst time.
3. **Zones skip full classes.** `ANPCZoneManager::IsValidNPCEntry` is hooked. When a zone weighs one of its
   spawn entries and every creature class in it is blocked or at its limit (or its group is), the entry is
   refused and the zone picks another one, so it spawns other species instead of creatures that would only
   be removed. The spawn check above stays as the backstop for anything that still gets through. Zones that
   keep spawning over-limit creatures anyway are held back; see [Limits and churn](limits-and-churn.md).
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

Under-mesh checks, block areas and the other location rules run alongside this cycle; see
[Under-mesh and block areas](under-mesh.md).

## What is left alone

- **Owned:** any creature on a player or tribe team (team 50000 and up), so all tames.
- **Mission:** any creature whose `OwnerMission` is set. On ASA this covers outpost garrisons and raids on
  Lost Colony and missions on Genesis maps; on ASE, Genesis Part 1 and 2 missions. They are left alone even
  when they use the same classes as ordinary wild creatures.
- **Protected:** classes whose path contains a `Protect.PathContains` entry (`/Missions/` by default) or that
  derive from a `Protect.Families` class.
- **Buff-protected:** wild creatures carrying a buff matched by `Protect.Buffs`, regardless of species.
  They skip ordinary culling and rolling refresh and do not count toward population rules. The existing
  under-mesh checks and confirmed removals still apply. See [Protect](config-reference.md#protect).
- **Summoned:** with `IgnoreSummoned` (the default), creatures that appear after the first census without a
  spawn zone spawning them (a Gloon's Revenant escorts, creatures a mod spawns around another one). They are
  counted and shown in `List` but outside every rule; limit the summoner instead.

## Reloads

`SpawnControl.Reload` re-resolves every class and clears the removal queue. Live counts carry over, so limits
keep holding; blocked classes and density rules are applied again by the next census. Zones held back for
churning stay held. Loading a new build loads the config too, so no reload is needed after a deploy.
