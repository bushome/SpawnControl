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
count per class, and shared maximums for groups of classes (a creature and its modded variant). Only wild
creatures are touched. Tamed and player-owned creatures, and creatures that belong to a mission or outpost,
are left alone. Removals are spread out a few per second, farthest from players first, so a large cull never
lands in one server tick.

Status: first version, not yet run on a server. Census, rules, removal queue and the `List`/`Reload` commands
are in; per-zone caps, mesh detection, spawners and the geometry probe are planned next.

| Folder | Game | Server API (submodule) | Map section picked by |
|---|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` | map name (`_WP` optional) or `-SpawnControlMap=` |
| [`ASE`](ASE/README.md) | ARK: Survival Evolved | [ArkServerApi (AseApi)](https://github.com/ArkServerApi/AseApi) at `ASE/extern/AseApi` | map name or `-SpawnControlMap=` |

Each folder is a self-contained Visual Studio solution. Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/SpawnControl.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

The rule logic (`src/SpawnLogic.h`), the tests and the configs are the same in both folders; only
`src/Main.cpp` and the project files differ by API. Build and install steps are in each folder's README.
