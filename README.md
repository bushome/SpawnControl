```
 _____                            _____             _             _
/  ___|                          /  __ \           | |           | |                   __
\ `--. _ __   __ ___      ___ __ | /  \/ ___  _ __ | |_ _ __ ___ | |                  / _)
 `--. \ '_ \ / _` \ \ /\ / / '_ \| |    / _ \| '_ \| __| '__/ _ \| |         _.----._/ /
/\__/ / |_) | (_| |\ V  V /| | | | \__/\ (_) | | | | |_| | | (_) | |        /         /
\____/| .__/ \__,_| \_/\_/ |_| |_|\____/\___/|_| |_|\__|_|  \___/|_|     __/ (  | (  |
      | |                                                               /__.-'|_|--|_|
      |_|
```
Server plugin that keeps wild creature populations in check, per map: classes that never spawn, a maximum
count per class, shared maximums for groups of classes, and caps on clumps and creatures spawned inside one
another. It finds creatures stuck under or inside the map with the game's own test and stops the spawn points
that keep producing them. Only wild creatures are touched: tamed and player-owned creatures, and creatures
that belong to a mission or outpost, are left alone. Removals are spread out a few per second, farthest from
players first, so a large cull never lands in one server tick, and spawn zones are steered away from classes
that are already full instead of feeding the removal queue.

On ASA it can also switch disabled spawn zones back on and spawn creatures where a map's own zones never do.

Status: running on a live ASA cluster and in first deployment on an ASE cluster.

**Documentation:** [`docs/`](docs/README.md) covers how it works, every setting, the commands and how to tune
a map. [`tools/Make-Limits.ps1`](tools/Make-Limits.ps1) turns a `ListDinos` survey into `Limit` entries.

| Folder | Game | Server API (submodule) | Map section picked by |
|---|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` | `-SpawnControlMap=`, `-ServerKey=` or map name (`_WP` optional) |
| [`ASE`](ASE/README.md) | ARK: Survival Evolved | [ArkServerApi (AseApi)](https://github.com/ArkServerApi/AseApi) at `ASE/extern/AseApi` | `-SpawnControlMap=`, `-ServerKey=` or map name |

Each folder is a self-contained Visual Studio solution. Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/SpawnControl.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

Both builds share the rule logic (`src/SpawnLogic.h`). The shipped configs use each game's Shiny mod path
in `Protect.Buffs`. Build and install steps are in each folder's README. See
[ASA and ASE differences](docs/asa-vs-ase.md) for what each build supports.

## Development

Developed with AI assistance (Claude Opus 5.5 and Codex GPT 6.2 Sol). Every change was reviewed and tested on a dedicated test server
before going live.
