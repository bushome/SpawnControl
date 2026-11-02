# SpawnControl documentation

These pages cover both builds; where ASA and ASE differ, the page says so. Build and install steps are in each
build's own README: [ASA](../ASA/README.md), [ASE](../ASE/README.md).

| Page | What it covers |
|---|---|
| [How it works](how-it-works.md) | The census, spawn check, zone refusal, removal queue and rolling refresh; which creatures are never touched. |
| [Config reference](config-reference.md) | `config.json`, SharedConfig folders, `Default`, every General, Protect and Refresh setting, how a map section is picked, every per-map rule, class keys. |
| [Commands](commands.md) | Every console and RCON command, and how to read `SpawnControl.List`. |
| [Limits and churn](limits-and-churn.md) | Choosing caps, surveying a map with `ListDinos` and [`tools/Make-Limits.ps1`](../tools/Make-Limits.ps1), applying rounds, the churn guard. |
| [Under-mesh and block areas](under-mesh.md) | Finding and removing creatures under the map, reading suspects, repeat spots, block areas, AutoBlockRepeats, the Genesis 1 ocean band. |
| [Zones and spawn points](zones-and-spawn-points.md) | ASA only: diagnosing zones that spawn too little, switching disabled zones back on, spawning creatures where zones never do. |
| [ASA and ASE differences](asa-vs-ase.md) | What each build supports, moving a config from ASA to ASE, notes for developers. |

## Quick start

1. Install the build for your game (see its README) and start the server.
2. Run `SpawnControl.List`: the first line names the config section in use. Add one if it says `none`.
3. Let the map settle, survey it with `ListDinos` and `Make-Limits.ps1`, and paste the limits in.
4. Turn on `UnderMesh` in report mode, check what it finds, then switch on `Remove` and `AutoBlockRepeats`.
