# SpawnControl (ASA)

An [AsaApi](https://github.com/ArkServerApi/AsaApi) plugin for ARK: Survival Ascended servers. It keeps wild
creature populations in check per map: blocked classes, a maximum per class, and shared maximums for groups
of classes. Tamed, player-owned and mission or outpost creatures are never touched.

The ARK: Survival Evolved build lives in `..\ASE`.

## Documentation

How the plugin works, every setting, the commands and how to tune a map are in [`../docs`](../docs/README.md),
shared with the ASE build:

- [How it works](../docs/how-it-works.md)
- [Config reference](../docs/config-reference.md)
- [Commands](../docs/commands.md)
- [Limits and churn](../docs/limits-and-churn.md)
- [Under-mesh and block areas](../docs/under-mesh.md)
- [Zones and spawn points](../docs/zones-and-spawn-points.md)
- [ASA and ASE differences](../docs/asa-vs-ase.md)

## Build

**Shared API checkout (optional):** the project builds against `extern\AsaApi` by default. To build several plugins against one shared AsaApi checkout instead, set the MSBuild property `AsaApiDir` to that folder, for example in a `Directory.Build.props` placed above the repo; the submodule then doesn't need to be checked out.

Requirements: Visual Studio 2026 with the "Desktop development with C++" workload (v145 toolset), and Git.

AsaApi is a git submodule at `extern\AsaApi`. After cloning the repository:

```
git submodule update --init --recursive
```

1. Build `extern\AsaApi\AsaApi.sln` (**Release / x64**) so `AsaApi.lib` lands in `extern\AsaApi\out_lib\`,
   or copy `AsaApi.lib` there from the `Lib` folder of the AsaApi release zip your servers run.
2. Open `SpawnControl.sln`, select **Release / x64**, build.

`fmt` comes from vcpkg through the manifest in `vcpkg.json` (Visual Studio's bundled vcpkg; triplet
`x64-windows-static-md`). AsaApi's `UnrealString.h` includes `fmt/format.h`, so it has to be on the include
path.

The result is `out\SpawnControl\`, containing `SpawnControl.dll`, its `.pdb`, `PluginInfo.json`, `config.json`,
`config.commented.json` and `THIRD-PARTY-NOTICES.md`. Keep the notice file with the DLL when redistributing
the plugin.

## Install

**VC++ runtime:** built with the v145 toolset (Visual Studio 2026) and `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`, the same as AsaApi, so it also runs on servers with an older Visual C++ runtime. If the server crashes inside `std::mutex` or reports a missing `msvcp140.dll` entry point after installing, update the VC++ runtime DLLs in `ShooterGame\Binaries\Win64` (the latest files are in [ASABinaries](https://github.com/bushome/ASABinaries)) or install the current Visual C++ Redistributable (x64).

The server machine needs a Microsoft Visual C++ x64 Redistributable at least as new as the Visual Studio
the plugin was built with.

1. If another mod or plugin limits or blocks spawns on the same map, take it out first, or both will act on
   the same creatures.
2. Copy the `out\SpawnControl` folder to `ShooterGame\Binaries\Win64\ArkApi\Plugins\`. The folder and the DLL
   must keep the same name.
3. Start the server. `ArkApi.log` shows `Config loaded: ... using <section> for map <name>`. About a minute
   after the world is up the first census finishes; if anything is over its rules, the log shows
   `Census: ... N over the rules (...), M newly queued, queue Q`, and once a minute
   `Removed in the last 60 s: total (per class, largest first)` while removals happen.

To update a running server, drop the new build in as `SpawnControl.dll.arkapi`. If automatic plugin
reloading is enabled in AsaApi's own `config.json`, AsaApi swaps it in.

## First run on each map

1. Run `SpawnControl.List`. The first line shows the map name the server reports and which config section is
   in use. If it says `section none`, add a section under `Maps` with that name (or the name without `_WP`)
   and run `SpawnControl.Reload`.
2. `SpawnControl.List verbose` ends with the ten largest classes that have no rule. These are candidates for a limit.
3. `SpawnControl.List <text>` shows every class whose path contains the text, with wild and mission counts
   and the full path to paste into the config, e.g. `SpawnControl.List otter`.
4. Once the map has settled, survey it and set limits as in [Limits and churn](../docs/limits-and-churn.md).

## Troubleshooting

- **`Config not loaded: ...` at startup.** The message names the problem. Until it is fixed and
  `SpawnControl.Reload` is run, nothing is limited.
- **`section none` in `List`.** No `Maps` section matches the map name shown on the same line.
- **`Could not hook ANPCZoneManager.IsValidNPCEntry...`.** The AsaApi on the server does not know that function.
  Limits still hold; zones just keep spawning full classes for the queue to remove, so the per-minute
  `Removed` line stays busier.
- **`Could not hook APrimalDinoCharacter.BeginPlay()...`.** The AsaApi on the server does not know that
  function, usually because it is out of date for the game version. The census still enforces every rule,
  only less promptly.
- **Checking for hitches without a player online.** See [Commands](../docs/commands.md#reading-spawncontrollist).
- **A count sits slightly over its limit.** Spawns between two census passes are checked against counts that
  include creatures killed since the last pass, so a class can float a little over or under its limit; each
  pass corrects it.

## Tests

`tests\logic_tests.cpp` covers config parsing, map section lookup, class rules and the removal plan. It needs
no game and no Windows. From a Visual Studio developer prompt in this folder:

```
cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp
logic_tests.exe configs\config.json --shipped
logic_tests.exe configs\config.commented.json --shipped
```

Passing a config file validates it and prints any warnings, a quick way to check an edit before putting it
on a server. `--shipped` also checks it against the shipped values.

## Files

| File | Purpose |
|---|---|
| `src\Main.cpp` | Census, spawn hook, removal queue, commands. |
| `src\SpawnLogic.h` | Config parsing, class rules, removal plan. No game dependencies. Same file in `..\ASE`. |
| `configs\config.json` | Default configuration with the Lost Colony section. |
| `configs\config.commented.json` | The same configuration with every setting explained. Not read by the plugin. |
| `configs\PluginInfo.json` | Plugin metadata read by AsaApi. |
| `tests\logic_tests.cpp` | Stand-alone tests for `SpawnLogic.h` and the shipped config. |
| `vcpkg.json` | vcpkg manifest (fmt). |
| `extern\AsaApi` | AsaApi submodule the plugin builds against. |
