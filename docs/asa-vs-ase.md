# ASA and ASE differences

[Back to the docs index](README.md)

Both builds share the rule logic in `src/SpawnLogic.h` and read the same config format. The shipped
`Protect.Buffs` list uses the Shiny mod path for each game. `src/Main.cpp` and the project files differ,
and some features exist only on ASA because the ASE API lacks what they need or because they were written
for ASA maps.

| Feature | ASA | ASE |
|---|---|---|
| Limits, groups, blocks, density, overlap, refresh | yes | yes |
| Zone entry refusal and churn guard | yes | yes |
| `UnderMesh` with `IgnoreFoliage`, `TraceGround`, `RemoveNoGround`, `StopZones` | yes | yes |
| Map exclusion zones skipped by `UnderMesh` (caves marked by the mapmaker) | yes | no API for it; check cave suspects by hand |
| Wild followers removed with an under-mesh creature | yes | no follower list; tested on their own |
| `DensitySkip.Caves` | yes | no cave flag; use `DensitySkip.Areas` |
| `BlockAreas`, `AutoBlockRepeats` | yes | yes |
| `AsaGen1OceanBand` | yes | ignored with a warning |
| `EnableZones`, `DisableZones`, `AutoDisableZones`, `SpawnPoints` | yes | ignored with a warning |
| `SpawnControl.Zones`, `SpawnControl.Links` | yes | no |
| `List`'s Failing zones section | yes | no (no zone tooling) |
| Mission creatures left alone | Lost Colony outposts, Genesis missions | Genesis Part 1 and 2 missions |
| Map name match | `_WP` suffix may be left off | as reported |
| Hot-reload file name | `SpawnControl.dll.arkapi` | `SpawnControl.dll.ArkApi` |
| vcpkg | `fmt` | none |

## Moving a config from ASA to ASE

- **Limits** mostly carry over, since most class names match. Drop classes from mods the ASE servers do not
  run (a class that matches nothing is simply never counted), then re-survey each map once it has run for a
  while; populations differ between the games.
- **Block areas, `<Map>.auto.json` files and coordinates** are tied to ASA geometry: drop them all. Keep
  `AutoBlockRepeats` enabled so ASE builds its own.
- **ASA-only settings:** drop `AsaGen1OceanBand`, `EnableZones`, `DisableZones`, `AutoDisableZones` and `SpawnPoints`.
- **Section names:** ASE map names differ (no `_WP`, some end in `_P`). Keying sections by `-ServerKey=` and
  naming the map files after the server keys avoids the problem. Check each server's load line or
  `SpawnControl.Rules` after the first start.

## For developers

- Keep `SpawnLogic.h` and `tests/logic_tests.cpp` byte-identical in both folders.
- ASE gets locations with `ActorLocation(actor)`; its `VTraceSingleBP` takes non-const vectors and no
  debug-draw argument.
- The ASE build treats `sscanf` as an error (C4996); use `std::from_chars` or similar there.
- When porting a change to both `Main.cpp` files, apply it by exact-text replacement to each; a context-free
  patch can land in the wrong place.
