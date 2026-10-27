// SpawnControl - keeps wild creature populations in check per map: blocked classes, per-class limits and
// shared group limits, without touching tamed, player-owned or mission creatures.
//
// How it works:
//   - A census walks the level actor lists a slice per second (CensusActorsPerTick), counts every wild
//     creature by class and, at the end of each pass, plans which ones to remove so every rule holds.
//     Blocked classes go first, then the creatures farthest from any player.
//   - APrimalDinoCharacter::BeginPlay is hooked. Once a census has finished, a creature that appears over
//     its limit (or of a blocked class) is queued straight away instead of waiting for the next pass.
//   - ANPCZoneManager::IsValidNPCEntry is hooked. When a zone weighs a spawn entry whose creatures are all
//     blocked or at their limit, the entry is refused, so the zone spawns something else instead of a
//     creature that would only be removed again (RefuseFullEntries).
//   - Refresh.TurnoverHours (off by default) replaces wild creatures a few at a time, oldest first, so the
//     zones respawn them with fresh levels instead of a full wipe. Each census queues its share; nothing
//     near a player, recently hurt, unconscious, being tamed or young is refreshed.
//   - Removals never happen in the hook or in bulk: a queue drains RemovalsPerTick creatures per second,
//     each re-checked before it is destroyed (still wild, still not a mission creature, rule still set).
//
// Everything runs on the game thread (the hook, AsaApi's one-second timer callback and the commands), so
// the globals below need no locking.

#include "API/ARK/Ark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <random>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SpawnLogic.h"

#ifndef PROJECT_NAME
#define PROJECT_NAME "SpawnControl"
#endif

namespace
{
	using namespace SpawnControl;
	using Clock = std::chrono::steady_clock;

	const char* const kCmdReload = "SpawnControl.Reload";
	const char* const kCmdList = "SpawnControl.List";
	const char* const kCmdMeshHere = "SpawnControl.MeshHere";
	const char* const kCmdLinks = "SpawnControl.Links";
	const char* const kCmdRules = "SpawnControl.Rules";
	const char* const kTimerId = "SpawnControl.Timer";
	const char* const kHookEntry = "ANPCZoneManager.IsValidNPCEntry(FNPCSpawnEntry&)";
	const char* const kHookBeginPlay = "APrimalDinoCharacter.BeginPlay()";
	const char* const kHookZoneSpawn = "ANPCZoneManager.SpawnNPCs()";
	const char* const kLevelActorsField = "ULevel.Actors2";
	const wchar_t* const kMapSwitch = L"-SpawnControlMap=";
	const wchar_t* const kServerKeySwitch = L"-ServerKey=";  // shared with the other plugins

	// A queued creature that keeps turning up near players is removed anyway after this many deferrals.
	constexpr int kMaxNearPlayerDeferrals = 30;

	// ------------------------------------------------------------------------------------------
	// State
	// ------------------------------------------------------------------------------------------

	struct ClassEntry
	{
		ClassInfo info;
		ClassRule rule;
	};

	struct QueueItem
	{
		TWeakObjectPtr<APrimalDinoCharacter> creature;
		int class_index = 0;
		int deferrals = 0;
		bool refresh = false;  // a rolling refresh pick, not a rule removal
		bool area = false;     // inside a block area: removed whatever its class's rules
	};

	struct CensusItem
	{
		TWeakObjectPtr<APrimalDinoCharacter> creature;
		int class_index = 0;
		double player_distance = 0;
		double x = 0, y = 0, z = 0;
		bool in_cave = false;
		bool in_block_area = false;
		double radius = 0;
		bool attached = false;
	};

	struct Report
	{
		Clock::time_point finished{};
		double seconds = 0;
		size_t actors = 0;
		size_t wild = 0;
		size_t mission = 0;
		size_t summoned = 0;                  // wild, summoned by another creature, outside the rules
		size_t too_dense = 0;                 // planned by density rules
		size_t in_block_area = 0;             // planned because they are inside a block area
		size_t overlapping = 0;               // planned because they sit inside another creature
		double plan_ms = 0;                   // time to plan the removals at the end of the census
		std::vector<OverlapSample> overlap_samples;
		std::vector<std::vector<DensityHotspot>> hotspots; // fullest cubes per density rule
		std::vector<int> summoned_by_class;   // the summoned part of wild_by_class
		std::vector<int> wild_by_class;       // every class, protected included
		std::vector<int> mission_by_class;
		std::vector<size_t> family_wild;      // per report family, without a mission
		std::vector<size_t> family_mission;
		size_t planned = 0;
		size_t blocked = 0;
		size_t over_limit = 0;
		size_t over_group = 0;
	};

	Settings g_settings;
	bool g_settings_loaded = false;
	const MapRules* g_rules = nullptr; // points into g_settings
	std::string g_map_name;
	bool g_map_known = false;          // a map name was read once the world existed
	std::string g_section_source;      // how the section was chosen, for List

	std::vector<ClassEntry> g_classes;
	std::unordered_map<UClass*, int> g_class_index;

	// Live counts: set by each finished census, adjusted by spawns and removals in between.
	bool g_counts_ready = false;
	std::vector<int> g_class_counts;
	std::vector<int> g_group_counts;

	// Wild spawns the hook counted while a census was running. The census result replaces the live
	// counts when it finishes, so these are added back on top or they would be lost until the next pass.
	std::vector<int> g_spawned_during_census;

	std::deque<QueueItem> g_queue;
	std::unordered_set<uint64_t> g_queued; // weak pointer keys already in the queue

	// Census in progress.
	bool g_census_running = false;
	int g_seconds_since_census = 0;
	size_t g_census_level = 0;
	int32 g_census_actor = 0;
	Clock::time_point g_census_started{};
	std::vector<CensusItem> g_census_items;
	std::vector<FVector> g_player_positions;
	Report g_building;
	Report g_last;
	bool g_have_report = false;

	// Rolling refresh: when each wild creature was first seen (seconds since load), carried from census to
	// census by weak pointer key, and the creatures this census found eligible.
	struct RefreshItem
	{
		TWeakObjectPtr<APrimalDinoCharacter> creature;
		int class_index = 0;
		uint64_t key = 0;
		double player_distance = 0;
	};
	std::unordered_map<uint64_t, double> g_first_seen;
	std::unordered_map<uint64_t, double> g_first_seen_building;
	std::vector<RefreshItem> g_refresh_items;
	double g_refresh_carry = 0;
	double g_last_refresh_plan = -1;
	size_t g_refreshed_total = 0;
	std::unordered_map<int, size_t> g_refreshed_since_log;
	const Clock::time_point g_loaded_at = Clock::now();

	// Statistics.
	size_t g_removed_total = 0;
	size_t g_removed_skipped = 0; // gone, owned or protected by the time their turn came
	std::unordered_map<int, size_t> g_removed_since_log;
	int g_seconds_since_log = 0;
	constexpr int kLogIntervalSeconds = 60;
	double g_hook_total_us = 0;
	double g_hook_max_us = 0;

	// Time the timer callback spends on each job, in milliseconds. This is the whole of the plugin's
	// per-tick cost, so a hitch caused by it shows up here as a high max.
	struct Timing
	{
		double total_ms = 0;
		double max_ms = 0;
		size_t ticks = 0;
		void Add(double ms)
		{
			total_ms += ms;
			max_ms = (std::max)(max_ms, ms);
			++ticks;
		}
		double Average() const { return ticks > 0 ? total_ms / ticks : 0.0; }
	};
	Timing g_census_timing;  // ticks that ran a census slice
	Timing g_drain_timing;   // ticks that removed at least one creature
	size_t g_hook_calls = 0;

	bool g_hook_installed = false;
	bool g_zone_hook_installed = false;
	int g_zone_spawn_depth = 0;                         // > 0 while a zone is spawning its creatures
	uint64_t g_zone_spawn_calls = 0;
	ANPCZoneManager* g_current_zone = nullptr;          // the zone whose SpawnNPCs is running

	struct ZoneStats
	{
		int inside = 0;       // spawned into a block area
		int outside = 0;
		bool stopped = false;
		std::string area;     // the block area it spawned into
		int under = 0;        // its creatures removed under the mesh
		int clear = 0;        // its creatures that tested clear
		bool by_mesh = false; // stopped for spawning under the mesh, not into a block area
		double x = 0, y = 0, z = 0; // where its last counted creature was: zone managers have no useful location
	};

	// Zones whose spawns are almost all removed on arrival for being over a limit (see ChurnWindow).
	struct ZoneChurn
	{
		ChurnWindow window;
		std::vector<int> classes;   // classes it spawned over their limit, newest kept
		bool holding = false;       // spawning held back while those classes are full
		int holds = 0;              // times it was held back since the last reload
		double x = 0, y = 0, z = 0;
	};
	std::unordered_map<ANPCZoneManager*, ZoneChurn> g_zone_churn;
	uint64_t g_churn_held_calls = 0;      // zone spawn passes skipped for churning zones
	uint64_t g_entry_unloaded_class = 0;  // entries that could not be judged: a class not loaded through that entry
	// Zones that spawned into a block area or under the mesh since the last reload.
	std::unordered_map<ANPCZoneManager*, ZoneStats> g_zone_stats;
	size_t g_zones_stopped = 0;        // by block areas
	size_t g_mesh_zones_stopped = 0;   // by under-mesh removals

	// The zone each creature spawned from, while UnderMesh.StopZones is on. Pruned each census.
	struct CreatureOrigin
	{
		ANPCZoneManager* zone = nullptr;
		bool counted_clear = false;
		uint32_t last_seen = 0;   // census number
	};
	std::unordered_map<uint64_t, CreatureOrigin> g_creature_zone;

	// Under-mesh report: each census queues the creatures to test; the timer tests the awake ones at
	// UnderMesh.ChecksPerSecond with the game's own under-mesh test. Nothing is removed.
	struct MeshCandidate
	{
		TWeakObjectPtr<APrimalDinoCharacter> creature;
		int class_index = 0;
	};
	struct MeshSuspect
	{
		int class_index = 0;
		double x = 0, y = 0, z = 0;
		bool overlapping = false;   // the game says it overlaps geometry, not only that it is below it
		bool in_stasis = false;     // asleep, far from any player, when tested
		std::string hit;            // the kind of component the test hit
	};
	constexpr size_t kMeshRecent = 10;
	constexpr size_t kMeshPopsPerSecond = 5000;     // stasised creatures are skipped cheaply, but not without end
	constexpr size_t kMeshLogsPerMinute = 20;          // report mode: suspect lines; removing: removal lines
	std::deque<MeshCandidate> g_mesh_queue;
	std::deque<MeshSuspect> g_mesh_recent;
	std::unordered_set<uint64_t> g_mesh_reported;
	uint64_t g_mesh_checks = 0;
	uint64_t g_mesh_suspects = 0;
	std::unordered_map<int, uint32_t> g_mesh_suspects_by_class;   // class index -> suspects since load
	uint64_t g_mesh_in_exclusion_zone = 0;
	double g_mesh_total_us = 0;
	double g_mesh_max_us = 0;
	size_t g_mesh_logged_this_minute = 0;
	size_t g_mesh_unlogged_this_minute = 0;            // removals past the per-minute line cap, summed in one line
	uint32_t g_census_number = 0;                       // finished censuses since load
	std::unordered_map<uint64_t, uint32_t> g_mesh_first_below; // creature -> census it was first found below in
	uint64_t g_mesh_removal_queued = 0;                 // creatures (and their followers) queued for removal
	std::deque<RemovalPoint> g_mesh_removal_points;     // where under-mesh removals happened, newest last
	constexpr size_t kRemovalPointsKept = 1500;
	constexpr uint32_t kMeshStrikeCensuses = 30;          // a first "below" is kept this many censuses for its second
	constexpr double kRepeatSpotReach = 10 * kFoundation;
	constexpr int kRepeatSpotMinimum = 3;
	uint64_t g_stopped_zone_refusals = 0;
	uint64_t g_block_area_queued = 0;                   // creatures queued for being inside a block area
	uint64_t g_summoned_seen = 0;                       // creatures marked summoned since load
	std::unordered_set<uint64_t> g_summoned;            // wild creatures that appeared outside a zone spawn
	std::unordered_set<uint64_t> g_summoned_building;   // the same, seen by the census in progress
	bool g_entry_hook_installed = false;

	// Zone spawn entries weighed and refused, and the time spent deciding.
	size_t g_entry_checks = 0;
	size_t g_entry_refused = 0;
	double g_entry_total_us = 0;
	double g_entry_max_us = 0;
	std::unordered_map<int, size_t> g_refused_since_log;  // first full class of each refused entry

	// ------------------------------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------------------------------

	std::string ToUtf8(const FString& text)
	{
		return AsaApi::Tools::Utf8Encode(std::wstring(*text));
	}

	UWorld* World()
	{
		return AsaApi::GetApiUtils().GetWorld();
	}

	template <typename T>
	uint64_t WeakKey(const TWeakObjectPtr<T>& weak)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(weak.ObjectIndex)) << 32) |
			static_cast<uint32_t>(weak.ObjectSerialNumber);
	}

	double DistanceSquared(const FVector& a, const FVector& b)
	{
		const double dx = a.X - b.X;
		const double dy = a.Y - b.Y;
		const double dz = a.Z - b.Z;
		return dx * dx + dy * dy + dz * dz;
	}

	std::vector<FVector> PlayerPositions()
	{
		std::vector<FVector> positions;
		UWorld* world = World();
		if (world == nullptr)
			return positions;
		for (TWeakObjectPtr<APlayerController> controller : world->PlayerControllerListField())
		{
			APlayerController* player = controller.Get();
			if (player != nullptr && player->PawnField().Get() != nullptr)
				positions.push_back(AsaApi::IApiUtils::GetPosition(player));
		}
		return positions;
	}

	double NearestPlayerDistance(const FVector& location, const std::vector<FVector>& players)
	{
		if (players.empty())
			return 1.0e12; // nobody online: everything counts as far away
		double best = -1;
		for (const FVector& player : players)
		{
			const double d = DistanceSquared(location, player);
			if (best < 0 || d < best)
				best = d;
		}
		return std::sqrt(best);
	}

	double SecondsSinceLoad()
	{
		return std::chrono::duration<double>(Clock::now() - g_loaded_at).count();
	}

	// May this wild creature be refreshed right now? Not while unconscious, being tamed, recently hurt or
	// young (wild babies follow a parent).
	bool RefreshAllowedNow(APrimalDinoCharacter* dino)
	{
		if (!dino->IsConscious() || dino->TamingTeamIDField() != 0)
			return false;
		const float baby_age = dino->BabyAgeField();
		if (baby_age > 0.0f && baby_age < 1.0f)
			return false;
		if (dino->WildFollowingParentRefField() != nullptr)
			return false;
		UWorld* world = World();
		if (world != nullptr && g_settings.refresh_recent_damage_seconds > 0 && dino->LastTookDamageTimeField() > 0 &&
			world->TimeSecondsField() - dino->LastTookDamageTimeField() < g_settings.refresh_recent_damage_seconds)
			return false;
		return true;
	}

	// Class entry for a creature class: path, name chain and resolved rule, built once and cached.
	int ClassIndexFor(UClass* cls)
	{
		const auto found = g_class_index.find(cls);
		if (found != g_class_index.end())
			return found->second;

		ClassEntry entry;
		int depth = 0;
		for (UStruct* current = cls; current != nullptr && depth < 32; current = current->SuperStructField(), ++depth)
			entry.info.chain.push_back(current->NameField().ToString().ToString());
		entry.info.path = ClassPathFromReference(AsaApi::IApiUtils::GetClassBlueprint(cls).ToString());
		entry.info.short_name = !entry.info.chain.empty() ? entry.info.chain.front() : ShortNameFromPath(entry.info.path);
		if (entry.info.path.empty())
			entry.info.path = entry.info.short_name;
		entry.rule = ResolveClass(g_settings, g_rules, entry.info);

		const int index = static_cast<int>(g_classes.size());
		g_classes.push_back(std::move(entry));
		g_class_index.emplace(cls, index);
		g_class_counts.push_back(0);
		return index;
	}

	enum class Kind
	{
		Other,    // not a wild creature: tamed, owned, dead
		Mission,  // wild but belongs to a mission or outpost
		Summoned, // wild but summoned by another creature (a Gloon's escort); outside the rules
		Wild,
	};

	// Summoned by another creature: it appeared after startup without a zone spawning it. A Gloon's
	// Revenant escorts do; their summoner keeps no link the API can read, so where they came from is
	// the test. Removing one only makes its summoner replace it.
	int InBlockArea(const FVector& location)
	{
		if (g_rules == nullptr || g_rules->block_areas.empty())
			return -1;
		return BlockAreaAt(g_rules->block_areas, static_cast<double>(location.X), static_cast<double>(location.Y),
			static_cast<double>(location.Z));
	}

	bool IsSummoned(APrimalDinoCharacter* dino)
	{
		return !g_summoned.empty() && g_summoned.count(WeakKey(GetWeakReference(dino))) > 0;
	}

	void NoteAppearance(APrimalDinoCharacter* dino)
	{
		if (!g_zone_hook_installed || !g_settings.ignore_summoned || !g_have_report || g_zone_spawn_depth > 0)
			return; // before the first census, creatures are still loading from the save
		if (dino == nullptr || dino->TargetingTeamField() >= kFirstPlayerTeam || dino->OwnerMissionField() != nullptr)
			return;
		const uint64_t key = WeakKey(GetWeakReference(dino));
		if (g_summoned.insert(key).second)
			++g_summoned_seen;
		g_summoned_building.insert(key);
	}

	Kind Classify(APrimalDinoCharacter* dino)
	{
		if (dino == nullptr || dino->IsDead() || dino->bActorIsBeingDestroyed()())
			return Kind::Other; // dead or on its way out: its fields (bounds among them) are no longer trustworthy
		if (dino->TargetingTeamField() >= kFirstPlayerTeam)
			return Kind::Other;
		if (dino->OwnerMissionField() != nullptr)
			return Kind::Mission;
		if (g_settings.ignore_summoned && IsSummoned(dino))
			return Kind::Summoned;
		return Kind::Wild;
	}

	// ------------------------------------------------------------------------------------------
	// Config
	// ------------------------------------------------------------------------------------------

	// Value of a -Name=value switch on the server command line, or empty. The name matches in any case
	// (-ServerKey=, -serverkey=), the same way the other plugins read it.
	std::string SwitchValue(const wchar_t* name)
	{
		const std::wstring line = GetCommandLineW();
		std::wstring lower = line;
		std::wstring needle = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
		std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
		const size_t at = lower.find(needle);
		if (at == std::wstring::npos)
			return {};
		size_t start = at + needle.size();
		size_t end = std::wstring::npos;
		if (start < line.size() && line[start] == L'"')
		{
			++start;
			end = line.find(L'"', start);
		}
		else
		{
			end = line.find_first_of(L" \t\"?", start);
		}
		return AsaApi::Tools::Utf8Encode(line.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
	}

	std::string CurrentMapName()
	{
		UWorld* world = World();
		if (world == nullptr)
			return {};
		FString name;
		world->GetMapName(&name);
		return ToUtf8(name);
	}

	// Picks the map section and re-resolves every known class against it.
	void AdjustCounts(int class_index, int delta);

	void ApplyRules()
	{
		g_map_name = CurrentMapName();
		const bool was_known = g_map_known;
		g_map_known = !g_map_name.empty();
		const std::string forced = SwitchValue(kMapSwitch);
		// -ServerKey=x as a flag, or ?ServerKey=x in the map URL; the other plugins accept both.
		std::string server_key = SwitchValue(kServerKeySwitch);
		if (server_key.empty())
			server_key = SwitchValue(L"?ServerKey=");
		if (!forced.empty())
		{
			g_rules = FindMapRules(g_settings, forced);
			g_section_source = "-SpawnControlMap=" + forced;
		}
		else if (!server_key.empty() && FindMapRules(g_settings, server_key) != nullptr)
		{
			g_rules = FindMapRules(g_settings, server_key);
			g_section_source = "-ServerKey=" + server_key;
		}
		else
		{
			g_rules = FindMapRules(g_settings, g_map_name);
			g_section_source = "map " + g_map_name;
		}
		if (g_rules == nullptr && g_settings.has_default)
		{
			g_rules = &g_settings.default_rules;
			g_section_source += ", which has no section of its own";
		}
		if (g_map_known && !was_known)
			Log::GetLog()->info("Map {}: using section {} ({})", g_map_name, g_rules != nullptr ? g_rules->name : std::string("none"),
				g_section_source);

		// A queued removal was planned under the old rules. Live counts treat queued creatures as gone,
		// so they are counted again before the queue is dropped.
		if (g_counts_ready)
		{
			for (QueueItem& item : g_queue)
			{
				if (!item.refresh && item.creature.Get() != nullptr)
					AdjustCounts(item.class_index, +1);
			}
		}
		g_queue.clear();
		g_queued.clear();
		g_zone_stats.clear(); // block areas may have changed; stopped zones are learned again
		g_mesh_queue.clear(); // classes may have left the under-mesh rule; the next census queues again
		g_zones_stopped = 0;
		g_mesh_zones_stopped = 0;
		g_zone_churn.clear(); // limits may have changed
		for (auto& [key, origin] : g_creature_zone)
			origin.counted_clear = false; // zone counts start over with the stats

		std::vector<bool> had_rule(g_classes.size(), false);
		for (size_t c = 0; c < g_classes.size(); ++c)
			had_rule[c] = HasAnyRule(g_classes[c].rule);
		for (ClassEntry& entry : g_classes)
			entry.rule = ResolveClass(g_settings, g_rules, entry.info);

		// Keep enforcing through a reload: classes that were counted keep their live count, classes that
		// just got a rule start from the last census, groups are summed from their members. The next
		// census replaces all of it with real counts. Before the first census there is nothing to keep.
		g_group_counts.assign(g_rules != nullptr ? g_rules->groups.size() : 0, 0);
		if (g_counts_ready && g_have_report)
		{
			g_class_counts.resize(g_classes.size(), 0);
			for (size_t c = 0; c < g_classes.size(); ++c)
			{
				if (!had_rule[c] && c < g_last.wild_by_class.size())
					g_class_counts[c] = g_last.wild_by_class[c] -
						(c < g_last.summoned_by_class.size() ? g_last.summoned_by_class[c] : 0);
				for (int g : g_classes[c].rule.groups)
				{
					if (g >= 0 && static_cast<size_t>(g) < g_group_counts.size())
						g_group_counts[g] += g_class_counts[c];
				}
			}
		}
		else
		{
			g_counts_ready = false; // limits wait for the first census to have real counts
		}
	}

	// The plugin's config.json, or the shared file or folder its SharedConfig points to. Comments are
	// allowed, and a comma before a closing ] or } is dropped with a warning.
	Settings LoadSettings()
	{
		return LoadConfigFiles(SpawnControl::detail::PathFromUtf8(AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME));
	}

	std::string SourcesText()
	{
		std::string text;
		for (const std::string& source : g_settings.sources)
			text += (text.empty() ? "" : ", ") + source;
		return text.empty() ? std::string("?") : text;
	}

	// Loads config.json. On failure the previous settings stay in force.
	bool ReloadSettings(std::string& error)
	{
		try
		{
			Settings settings = LoadSettings();
			for (const std::string& warning : settings.warnings)
				Log::GetLog()->warn("Config: {}", warning);
			g_settings = std::move(settings);
			g_settings_loaded = true;
			ApplyRules();
			Log::GetLog()->info("Config loaded from {}: {} map section(s){}; using {} for {}", SummarizeSources(g_settings.sources),
				g_settings.maps.size(), g_settings.has_default ? " and Default" : "",
				g_rules != nullptr ? g_rules->name : std::string("no section"), g_section_source);
			return true;
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Queue
	// ------------------------------------------------------------------------------------------

	// Returns false when the creature is already queued.
	bool Enqueue(APrimalDinoCharacter* dino, int class_index, bool refresh = false, bool area = false)
	{
		QueueItem item;
		item.creature = GetWeakReference(dino);
		item.class_index = class_index;
		item.refresh = refresh;
		item.area = area;
		const uint64_t key = WeakKey(item.creature);
		if (!g_queued.insert(key).second)
			return false;
		g_queue.push_back(item);
		return true;
	}

	void AdjustCounts(int class_index, int delta)
	{
		if (class_index < 0 || static_cast<size_t>(class_index) >= g_classes.size())
			return;
		g_class_counts[class_index] += delta;
		for (int g : g_classes[class_index].rule.groups)
		{
			if (g >= 0 && static_cast<size_t>(g) < g_group_counts.size())
				g_group_counts[g] += delta;
		}
	}

	// One summary line per minute: how many were removed and of which classes, largest first.
	std::string RefusedSummary()
	{
		std::vector<std::pair<size_t, int>> rows;
		size_t total = 0;
		for (const auto& [class_index, count] : g_refused_since_log)
		{
			rows.emplace_back(count, class_index);
			total += count;
		}
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::ostringstream text;
		text << total << " (";
		for (size_t i = 0; i < rows.size() && i < 8; ++i)
			text << (i == 0 ? "" : ", ") << rows[i].first << " " << g_classes[rows[i].second].info.short_name;
		text << ")";
		return text.str();
	}

	void LogRemovals()
	{
		if (g_mesh_unlogged_this_minute > 0)
		{
			if (g_settings.log_removals)
				Log::GetLog()->info("Under mesh: {} more removed in the last {} s without a line each; {} shows where they cluster",
					g_mesh_unlogged_this_minute, kLogIntervalSeconds, kCmdList);
			g_mesh_unlogged_this_minute = 0;
		}
		if (!g_refused_since_log.empty())
		{
			if (g_settings.log_removals)
				Log::GetLog()->info("Spawn entries refused in the last {} s: {}", kLogIntervalSeconds, RefusedSummary());
			g_refused_since_log.clear();
		}
		if (!g_refreshed_since_log.empty())
		{
			if (g_settings.log_removals)
			{
				size_t total = 0;
				for (const auto& [class_index, count] : g_refreshed_since_log)
					total += count;
				Log::GetLog()->info("Refreshed in the last {} s: {} wild creature(s) (rolling refresh, {} total)",
					kLogIntervalSeconds, total, g_refreshed_total);
			}
			g_refreshed_since_log.clear();
		}
		if (g_removed_since_log.empty())
			return;
		if (g_settings.log_removals)
		{
			std::vector<std::pair<size_t, int>> rows;
			size_t total = 0;
			for (const auto& [class_index, count] : g_removed_since_log)
			{
				rows.emplace_back(count, class_index);
				total += count;
			}
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

			std::ostringstream text;
			for (size_t i = 0; i < rows.size(); ++i)
				text << (i == 0 ? "" : ", ") << rows[i].first << " " << g_classes[rows[i].second].info.short_name;
			Log::GetLog()->info("Removed in the last {} s: {} ({}); queue {}", kLogIntervalSeconds, total, text.str(),
				g_queue.size());
		}
		g_removed_since_log.clear();
	}

	void DrainQueue()
	{
		const std::vector<FVector> players = g_queue.empty() ? std::vector<FVector>() : PlayerPositions();
		int budget = g_settings.removals_per_tick;
		size_t looked = 0;
		const size_t limit = g_queue.size();

		while (budget > 0 && !g_queue.empty() && looked < limit)
		{
			QueueItem item = g_queue.front();
			g_queue.pop_front();
			++looked;

			APrimalDinoCharacter* dino = item.creature.Get();
			const bool valid_class = item.class_index >= 0 && static_cast<size_t>(item.class_index) < g_classes.size();
			if (item.refresh)
			{
				// A refresh pick that no longer qualifies is dropped; the next census picks another.
				g_queued.erase(WeakKey(item.creature));
				if (dino == nullptr || !valid_class || Classify(dino) != Kind::Wild || g_classes[item.class_index].rule.is_protected ||
					SkipsRefresh(g_settings, g_classes[item.class_index].info) || !RefreshAllowedNow(dino) ||
					NearestPlayerDistance(dino->GetLocation(), players) < g_settings.refresh_player_distance)
					continue;
				dino->Destroy(false, true);
				if (g_counts_ready && HasAnyRule(g_classes[item.class_index].rule))
					AdjustCounts(item.class_index, -1);
				++g_refreshed_total;
				++g_refreshed_since_log[item.class_index];
				--budget;
				continue;
			}
			if (dino == nullptr || !valid_class || Classify(dino) != Kind::Wild || g_classes[item.class_index].rule.is_protected ||
				(!item.area && !HasAnyRule(g_classes[item.class_index].rule)))
			{
				g_queued.erase(WeakKey(item.creature));
				++g_removed_skipped;
				continue;
			}

			// Keep removals out of players' sight while there is anything else to do.
			if (item.deferrals < kMaxNearPlayerDeferrals &&
				NearestPlayerDistance(dino->GetLocation(), players) < g_settings.near_player_distance)
			{
				++item.deferrals;
				g_queue.push_back(item);
				continue;
			}

			g_queued.erase(WeakKey(item.creature));
			dino->Destroy(false, true);
			++g_removed_total;
			++g_removed_since_log[item.class_index];
			--budget;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Census
	// ------------------------------------------------------------------------------------------

	void StartCensus()
	{
		g_census_running = true;
		g_census_level = 0;
		g_census_actor = 0;
		g_census_started = Clock::now();
		g_census_items.clear();
		g_refresh_items.clear();
		g_first_seen_building.clear();
		g_player_positions = PlayerPositions();
		g_spawned_during_census.assign(g_classes.size(), 0);

		g_building = Report();
		g_building.family_wild.assign(g_settings.report_families.size(), 0);
		g_building.family_mission.assign(g_settings.report_families.size(), 0);
	}

	void CountCreature(APrimalDinoCharacter* dino)
	{
		const Kind kind = Classify(dino);
		if (kind == Kind::Other)
			return;

		const int class_index = ClassIndexFor(dino->ClassField());
		if (g_building.wild_by_class.size() < g_classes.size())
		{
			g_building.wild_by_class.resize(g_classes.size(), 0);
			g_building.mission_by_class.resize(g_classes.size(), 0);
			g_building.summoned_by_class.resize(g_classes.size(), 0);
		}
		const ClassRule& rule = g_classes[class_index].rule;

		if (kind == Kind::Mission)
		{
			++g_building.mission;
			++g_building.mission_by_class[class_index];
			if (rule.report_family >= 0)
				++g_building.family_mission[rule.report_family];
			return;
		}

		++g_building.wild;
		++g_building.wild_by_class[class_index];
		if (rule.report_family >= 0)
			++g_building.family_wild[rule.report_family];
		if (kind == Kind::Summoned)
		{
			g_summoned_building.insert(WeakKey(GetWeakReference(dino)));
			++g_building.summoned;
			++g_building.summoned_by_class[class_index];
			return;
		}

		if (g_settings.refresh_hours > 0 && !rule.is_protected && !SkipsRefresh(g_settings, g_classes[class_index].info))
		{
			RefreshItem refresh;
			refresh.creature = GetWeakReference(dino);
			refresh.key = WeakKey(refresh.creature);
			const auto seen = g_first_seen.find(refresh.key);
			g_first_seen_building[refresh.key] = seen != g_first_seen.end() ? seen->second : SecondsSinceLoad();
			if (RefreshAllowedNow(dino))
			{
				refresh.class_index = class_index;
				refresh.player_distance = NearestPlayerDistance(dino->GetLocation(), g_player_positions);
				g_refresh_items.push_back(refresh);
			}
		}

		const FVector location = dino->GetLocation();
		const bool in_block_area = InBlockArea(location) >= 0;
		if ((!HasAnyRule(rule) && !in_block_area) || rule.is_protected)
			return;

		CensusItem item;
		item.creature = GetWeakReference(dino);
		item.class_index = class_index;
		item.player_distance = NearestPlayerDistance(location, g_player_positions);
		item.x = static_cast<double>(location.X);
		item.y = static_cast<double>(location.Y);
		item.z = static_cast<double>(location.Z);
		item.in_cave = dino->bWasSpawnedInACaveOrBossArea()();
		item.in_block_area = in_block_area;
		if (rule.overlap)
		{
			// The root component is the collision capsule; its bounds' horizontal extent is the radius.
			if (USceneComponent* root = dino->RootComponentField().Get())
			{
				// FBoxSphereBounds is { FVector Origin; FVector BoxExtent; double SphereRadius; } in doubles.
				const double* bounds = reinterpret_cast<const double*>(&root->BoundsField());
				item.radius = (std::max)(bounds[3], bounds[4]);
				item.attached = root->AttachParentField().Get() != nullptr;
			}
		}
		g_census_items.push_back(item);
	}

	// Queues this census's share of the rolling refresh: enough that the wild population turns over in
	// Refresh.TurnoverHours, oldest first. Ages only count from plugin load.
	void PlanRefreshFromCensus()
	{
		g_first_seen.swap(g_first_seen_building);
		g_first_seen_building.clear();
		g_summoned.swap(g_summoned_building); // drops creatures that are gone
		g_summoned_building.clear();
		if (g_settings.refresh_hours <= 0)
		{
			g_refresh_items.clear();
			g_last_refresh_plan = -1;
			return;
		}

		const double now = SecondsSinceLoad();
		const double elapsed = g_last_refresh_plan < 0 ? 0 : now - g_last_refresh_plan;
		g_last_refresh_plan = now;
		const size_t quota = RefreshQuota(g_first_seen.size(), elapsed, g_settings.refresh_hours, g_refresh_carry);

		std::vector<RefreshCandidate> candidates;
		candidates.reserve(g_refresh_items.size());
		for (size_t i = 0; i < g_refresh_items.size(); ++i)
		{
			const auto seen = g_first_seen.find(g_refresh_items[i].key);
			const double age = seen != g_first_seen.end() ? now - seen->second : 0;
			candidates.push_back({ i, age, g_refresh_items[i].player_distance });
		}
		for (size_t i : PlanRefresh(std::move(candidates), quota, g_settings.refresh_min_age_minutes * 60.0,
			g_settings.refresh_player_distance))
		{
			APrimalDinoCharacter* dino = g_refresh_items[i].creature.Get();
			if (dino != nullptr)
				Enqueue(dino, g_refresh_items[i].class_index, true);
		}
		g_refresh_items.clear();
	}

	void FinishCensus()
	{
		g_census_running = false;
		g_seconds_since_census = 0;

		std::vector<ClassRule> rules;
		rules.reserve(g_classes.size());
		for (const ClassEntry& entry : g_classes)
			rules.push_back(entry.rule);

		std::vector<Creature> creatures;
		creatures.reserve(g_census_items.size());
		for (size_t i = 0; i < g_census_items.size(); ++i)
		{
			const CensusItem& item = g_census_items[i];
			creatures.push_back({ i, item.class_index, item.player_distance, item.x, item.y, item.z, item.in_cave, item.in_block_area,
				item.radius, item.attached });
		}

		static const std::vector<GroupRule> no_groups;
		static const std::vector<DensityRule> no_density;
		const std::vector<GroupRule>& groups = g_rules != nullptr ? g_rules->groups : no_groups;
		const std::vector<DensityRule>& density = g_rules != nullptr ? g_rules->density : no_density;
		static const DensitySkip default_skip;
		const DensitySkip& skip = g_rules != nullptr ? g_rules->density_skip : default_skip;
		const double overlap_fraction = g_rules != nullptr ? g_rules->overlap.fraction : 0.5;
		const auto plan_start = Clock::now();
		const Plan plan = PlanRemovals(rules, groups, creatures, density, skip, overlap_fraction);
		const double plan_ms = std::chrono::duration<double, std::milli>(Clock::now() - plan_start).count();

		// Live counts start from what each rule leaves after the planned removals.
		g_class_counts.assign(g_classes.size(), 0);
		for (size_t c = 0; c < plan.class_counts.size() && c < g_class_counts.size(); ++c)
			g_class_counts[c] = plan.class_counts[c];
		g_group_counts = plan.group_counts;
		for (size_t c = 0; c < g_spawned_during_census.size() && c < g_class_counts.size(); ++c)
		{
			if (g_spawned_during_census[c] != 0)
				AdjustCounts(static_cast<int>(c), g_spawned_during_census[c]);
		}
		g_spawned_during_census.clear();
		g_counts_ready = true;

		// Anything already queued is still on its way out; the plan counted it as present.
		size_t newly_queued = 0;
		for (size_t id : plan.remove)
		{
			APrimalDinoCharacter* dino = g_census_items[id].creature.Get();
			if (dino != nullptr && Enqueue(dino, g_census_items[id].class_index, false, g_census_items[id].in_block_area))
			{
				++newly_queued;
				if (g_census_items[id].in_block_area)
					++g_block_area_queued;
			}
		}

		g_building.finished = Clock::now();
		g_building.seconds = std::chrono::duration<double>(g_building.finished - g_census_started).count();
		g_building.planned = plan.remove.size();
		g_building.blocked = plan.blocked;
		g_building.over_limit = plan.over_limit;
		g_building.over_group = plan.over_group;
		g_building.too_dense = plan.too_dense;
		g_building.in_block_area = plan.in_block_area;
		g_building.overlapping = plan.overlapping;
		g_building.plan_ms = plan_ms;
		if (plan_ms > 50)
			Log::GetLog()->warn("Census planning took {:.0f} ms for {} creatures", plan_ms, creatures.size());
		g_building.overlap_samples = plan.overlap_samples;

		++g_census_number;
		if (!g_creature_zone.empty())
		{
			for (const CensusItem& item : g_census_items)
			{
				const auto origin = g_creature_zone.find(WeakKey(item.creature));
				if (origin != g_creature_zone.end())
					origin->second.last_seen = g_census_number;
			}
			// Gone since the census before last (spawns during a census carry the number they spawned at).
			for (auto it = g_creature_zone.begin(); it != g_creature_zone.end();)
				it = it->second.last_seen + 1 < g_census_number ? g_creature_zone.erase(it) : std::next(it);
		}
		for (auto it = g_mesh_first_below.begin(); it != g_mesh_first_below.end();)
			// Forgotten only after many censuses: on a map where tests are expensive (Aberration: ~260 us each) a low
			// ChecksPerSecond reaches each creature only every several censuses, and the second strike must still count.
			it = it->second + kMeshStrikeCensuses < g_census_number ? g_mesh_first_below.erase(it) : std::next(it);
		g_mesh_queue.clear();
		if (g_rules != nullptr && g_rules->under_mesh.enabled)
		{
			for (const CensusItem& item : g_census_items)
			{
				const ClassRule& rule = g_classes[item.class_index].rule;
				if (rule.under_mesh && !rule.is_protected && !item.in_cave && !item.in_block_area)
					g_mesh_queue.push_back({ item.creature, item.class_index });
			}
			// Shuffled, so when the budget does not reach the end before the next census, a different part
			// of the map goes untested each time instead of always the same one.
			static std::mt19937 shuffle_rng(std::random_device{}());
			std::shuffle(g_mesh_queue.begin(), g_mesh_queue.end(), shuffle_rng);
		}
		g_building.hotspots = plan.hotspots;
		g_building.wild_by_class.resize(g_classes.size(), 0);
		g_building.mission_by_class.resize(g_classes.size(), 0);
		g_building.summoned_by_class.resize(g_classes.size(), 0);
		g_last = g_building;
		g_have_report = true;
		g_census_items.clear();

		PlanRefreshFromCensus();

		if (plan.remove.size() > 0)
			Log::GetLog()->info("Census: {} wild ({} summoned), {} mission; {} over the rules ({} blocked, {} over limit, {} over group, "
				"{} too dense, {} overlapping, {} in block areas), {} newly queued, queue {}", g_last.wild, g_last.summoned, g_last.mission,
				plan.remove.size(), plan.blocked, plan.over_limit, plan.over_group, plan.too_dense, plan.overlapping, plan.in_block_area,
				newly_queued, g_queue.size());
	}

	// One slice of the census: up to CensusActorsPerTick level actors.
	void StepCensus()
	{
		UWorld* world = World();
		if (world == nullptr)
			return;

		UClass* dino_class = APrimalDinoCharacter::GetPrivateStaticClass();
		int budget = g_settings.census_actors_per_tick;
		auto& levels = world->LevelsField();

		while (budget > 0)
		{
			if (g_census_level >= static_cast<size_t>(levels.Num()))
			{
				FinishCensus();
				return;
			}

			ULevel* level = levels[static_cast<int32>(g_census_level)].Get();
			if (level == nullptr)
			{
				++g_census_level;
				g_census_actor = 0;
				continue;
			}

			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			while (budget > 0 && g_census_actor < actors.Num())
			{
				AActor* actor = actors[g_census_actor++];
				--budget;
				++g_building.actors;
				if (actor != nullptr && actor->IsA(dino_class))
					CountCreature(static_cast<APrimalDinoCharacter*>(actor));
			}
			if (g_census_actor >= actors.Num())
			{
				++g_census_level;
				g_census_actor = 0;
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Spawn hook
	// ------------------------------------------------------------------------------------------

	std::string ObjectName(ANPCZoneManager* zone)
	{
		UObject* object = reinterpret_cast<UObject*>(zone);
		return object != nullptr ? object->NameField().ToString().ToString() : std::string("?");
	}

	// Under-mesh zone stopping: on with UnderMesh Remove and StopZones, and only when spawns can be traced to
	// their zone.
	bool MeshZoneStopping()
	{
		return g_zone_hook_installed && g_rules != nullptr && g_rules->under_mesh.enabled && g_rules->under_mesh.remove &&
			g_rules->under_mesh.stop_zones;
	}

	void NoteMeshZoneClear(uint64_t creature_key)
	{
		const auto origin = g_creature_zone.find(creature_key);
		if (origin == g_creature_zone.end() || origin->second.counted_clear)
			return;
		origin->second.counted_clear = true;
		++g_zone_stats[origin->second.zone].clear;
	}

	// A creature removed under the mesh counts against the zone that spawned it; a zone whose creatures end up
	// there and hardly anywhere else is stopped.
	void NoteMeshZoneRemoval(uint64_t creature_key, const FVector& location)
	{
		const auto origin = g_creature_zone.find(creature_key);
		if (origin == g_creature_zone.end())
			return;
		ANPCZoneManager* const zone = origin->second.zone;
		g_creature_zone.erase(origin);
		if (!MeshZoneStopping())
			return;
		ZoneStats& stats = g_zone_stats[zone];
		++stats.under;
		stats.x = static_cast<double>(location.X);
		stats.y = static_cast<double>(location.Y);
		stats.z = static_cast<double>(location.Z);
		if (stats.stopped || !MeshZoneShouldStop(stats.under, stats.clear))
			return;
		stats.stopped = true;
		stats.by_mesh = true;
		++g_mesh_zones_stopped;
		Log::GetLog()->warn("Zone {} had {} of its creatures removed under the mesh and {} test clear; its spawn entries are "
			"refused from now on (last removal at {:.0f} {:.0f} {:.0f})", ObjectName(zone), stats.under, stats.clear, stats.x, stats.y,
			stats.z);
	}

	// Spawns by a zone into a block area, and the zone that keeps doing only that is stopped.
	void NoteZoneSpawn(ANPCZoneManager* zone, int area, const FVector& location)
	{
		ZoneStats& stats = g_zone_stats[zone];
		if (area >= 0)
		{
			++stats.inside;
			stats.area = g_rules->block_areas[area].name;
			stats.x = static_cast<double>(location.X);
			stats.y = static_cast<double>(location.Y);
			stats.z = static_cast<double>(location.Z);
		}
		else
			++stats.outside;
		if (!stats.stopped && ZoneShouldStop(stats.inside, stats.outside))
		{
			stats.stopped = true;
			++g_zones_stopped;
			Log::GetLog()->warn("Zone {} spawned {} creatures only inside block area {}; its spawn entries are refused from now on "
				"(last one at {:.0f} {:.0f} {:.0f})", ObjectName(zone), stats.inside, stats.area.empty() ? std::string("(unnamed)") : stats.area,
				stats.x, stats.y, stats.z);
		}
	}

	// Each zone spawn, kept or removed on arrival for being over a limit. A zone that only feeds the removal
	// queue is held back (Hook_ANPCZoneManager_SpawnNPCs) while the classes it churned are full.
	void NoteZoneOutcome(ANPCZoneManager* zone, int class_index, bool removed, const FVector& location)
	{
		ZoneChurn& churn = g_zone_churn[zone];
		churn.window.Add(removed);
		if (removed)
		{
			auto known = std::find(churn.classes.begin(), churn.classes.end(), class_index);
			if (known != churn.classes.end())
				churn.classes.erase(known);
			churn.classes.insert(churn.classes.begin(), class_index);
			if (churn.classes.size() > 4)
				churn.classes.pop_back();
			churn.x = static_cast<double>(location.X);
			churn.y = static_cast<double>(location.Y);
			churn.z = static_cast<double>(location.Z);
		}
		if (churn.holding || !churn.window.Churning())
			return;
		churn.holding = true;
		if (++churn.holds == 1)
		{
			std::string names;
			for (int c : churn.classes)
				names += (names.empty() ? "" : ", ") + g_classes[c].info.short_name;
			Log::GetLog()->warn("Zone {}: {} of its last {} spawns were over a limit and removed on arrival ({}); its spawning is held "
				"back while they are full (last one at {:.0f} {:.0f} {:.0f})", ObjectName(zone), churn.window.Removed(), kChurnWindow, names,
				churn.x, churn.y, churn.z);
		}
	}

	// A held zone resumes as soon as any class it churned has room again.
	bool ChurnStillFull(const ZoneChurn& churn)
	{
		if (!g_counts_ready || g_rules == nullptr || churn.classes.empty())
			return false;
		for (int c : churn.classes)
		{
			if (c < 0 || static_cast<size_t>(c) >= g_classes.size() || static_cast<size_t>(c) >= g_class_counts.size())
				return false;
			if (!IsFull(g_classes[c].rule, g_class_counts[c], g_group_counts, g_rules->groups))
				return false;
		}
		return true;
	}

	void OnCreatureAppeared(APrimalDinoCharacter* dino)
	{
		if (!g_settings_loaded || g_rules == nullptr || !g_counts_ready)
			return;
		if (Classify(dino) != Kind::Wild)
			return;

		const int class_index = ClassIndexFor(dino->ClassField());
		const ClassRule& rule = g_classes[class_index].rule;
		if (rule.is_protected)
			return;

		if (MeshZoneStopping() && g_zone_spawn_depth > 0 && g_current_zone != nullptr)
		{
			CreatureOrigin origin;
			origin.zone = g_current_zone;
			origin.last_seen = g_census_number;
			g_creature_zone[WeakKey(GetWeakReference(dino))] = origin;
		}

		if (!g_rules->block_areas.empty())
		{
			const int area = InBlockArea(dino->GetLocation());
			if (g_zone_spawn_depth > 0 && g_current_zone != nullptr)
				NoteZoneSpawn(g_current_zone, area, dino->GetLocation());
			if (area >= 0)
			{
				if (Enqueue(dino, class_index, false, true))
					++g_block_area_queued;
				return;
			}
		}
		ANPCZoneManager* const zone = g_zone_spawn_depth > 0 ? g_current_zone : nullptr;
		if (!HasAnyRule(rule))
		{
			if (zone != nullptr)
				NoteZoneOutcome(zone, class_index, false, dino->GetLocation());
			return;
		}

		// Live counts always mean "after everything already queued is gone".
		AdjustCounts(class_index, +1);
		bool kept = true;
		if (OverOnSpawn(rule, g_class_counts[class_index], g_group_counts, g_rules->groups))
		{
			if (Enqueue(dino, class_index))
			{
				AdjustCounts(class_index, -1);
				kept = false;
			}
		}
		if (zone != nullptr)
			NoteZoneOutcome(zone, class_index, !kept, dino->GetLocation());
		if (kept && g_census_running)
		{
			if (g_spawned_during_census.size() <= static_cast<size_t>(class_index))
				g_spawned_during_census.resize(class_index + 1, 0);
			++g_spawned_during_census[class_index];
		}
	}

	// True when every creature class an entry would spawn is blocked or at its limit. An entry with no
	// readable class, or with any class that still has room, is left to the game.
	bool EntryIsFull(FNPCSpawnEntry* entry, int& first_full)
	{
		first_full = -1;
		bool any = false;
		// NPCsToSpawn holds soft class pointers. Only their cached weak pointer is read: it never loads an
		// asset, and a class that is not loaded has no creatures yet, so it cannot be full.
		for (TSoftClassPtr<APrimalDinoCharacter>& soft : entry->NPCsToSpawn)
		{
			UObject* loaded = reinterpret_cast<FWeakObjectPtr*>(&soft)->Get(false);
			UClass* cls = static_cast<UClass*>(loaded);
			if (cls == nullptr)
			{
				++g_entry_unloaded_class;
				return false;
			}
			const int class_index = ClassIndexFor(cls);
			const ClassRule& rule = g_classes[class_index].rule;
			if (!IsFull(rule, g_class_counts[class_index], g_group_counts, g_rules->groups))
				return false;
			if (first_full < 0)
				first_full = class_index;
			any = true;
		}
		return any;
	}

	DECLARE_HOOK(ANPCZoneManager_IsValidNPCEntry, bool, ANPCZoneManager*, FNPCSpawnEntry*);

	bool Hook_ANPCZoneManager_IsValidNPCEntry(ANPCZoneManager* _this, FNPCSpawnEntry* entry)
	{
		const bool valid = ANPCZoneManager_IsValidNPCEntry_original(_this, entry);
		if (valid && !g_zone_stats.empty())
		{
			const auto zone = g_zone_stats.find(_this);
			if (zone != g_zone_stats.end() && zone->second.stopped)
			{
				++g_stopped_zone_refusals;
				return false;
			}
		}
		if (!valid || entry == nullptr || !g_settings_loaded || !g_settings.refuse_full_entries || g_rules == nullptr ||
			!g_counts_ready)
			return valid;

		const auto start = Clock::now();
		bool refuse = false;
		try
		{
			int first_full = -1;
			refuse = EntryIsFull(entry, first_full);
			if (refuse)
			{
				++g_entry_refused;
				++g_refused_since_log[first_full];
			}
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("Spawn entry check failed: {}", exception.what());
		}
		const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
		g_entry_total_us += us;
		g_entry_max_us = (std::max)(g_entry_max_us, us);
		++g_entry_checks;
		return refuse ? false : valid;
	}

	DECLARE_HOOK(ANPCZoneManager_SpawnNPCs, bool, ANPCZoneManager*);

	bool Hook_ANPCZoneManager_SpawnNPCs(ANPCZoneManager* _this)
	{
		if (!g_zone_churn.empty())
		{
			const auto churn = g_zone_churn.find(_this);
			if (churn != g_zone_churn.end() && churn->second.holding)
			{
				if (ChurnStillFull(churn->second))
				{
					++g_churn_held_calls;
					return false;
				}
				churn->second.holding = false;
			}
		}
		++g_zone_spawn_depth;
		++g_zone_spawn_calls;
		ANPCZoneManager* const outer_zone = g_current_zone;
		g_current_zone = _this;
		const bool result = ANPCZoneManager_SpawnNPCs_original(_this);
		g_current_zone = outer_zone;
		--g_zone_spawn_depth;
		return result;
	}

	DECLARE_HOOK(APrimalDinoCharacter_BeginPlay, void, APrimalDinoCharacter*);

	void Hook_APrimalDinoCharacter_BeginPlay(APrimalDinoCharacter* _this)
	{
		APrimalDinoCharacter_BeginPlay_original(_this);

		const auto start = Clock::now();
		try
		{
			NoteAppearance(_this);
			OnCreatureAppeared(_this);
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("Spawn check failed: {}", exception.what());
		}
		const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
		g_hook_total_us += us;
		g_hook_max_us = (std::max)(g_hook_max_us, us);
		++g_hook_calls;
	}

	// ------------------------------------------------------------------------------------------
	// Timer
	// ------------------------------------------------------------------------------------------

	void MeshTick();

	void OnTimer()
	{
		if (!g_settings_loaded || World() == nullptr)
			return;

		// The map name is only known once the world exists; pick the section then.
		if (!g_map_known)
			ApplyRules();

		if (g_census_running)
		{
			const auto start = Clock::now();
			StepCensus();
			g_census_timing.Add(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
		}
		else if (++g_seconds_since_census >= g_settings.census_interval_seconds || !g_have_report)
		{
			StartCensus();
		}

		if (++g_seconds_since_log >= kLogIntervalSeconds)
		{
			g_seconds_since_log = 0;
			g_mesh_logged_this_minute = 0;
			LogRemovals();
		}

		MeshTick();

		if (!g_queue.empty())
		{
			const size_t removed_before = g_removed_total;
			const auto start = Clock::now();
			DrainQueue();
			if (g_removed_total != removed_before)
				g_drain_timing.Add(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
		}
	}

	// A creature found below the surface on two censuses is queued for removal like a block-area creature.
	void RemoveUnderMesh(APrimalDinoCharacter* dino, int class_index, const FVector& location)
	{
		size_t queued = 0;
		if (Enqueue(dino, class_index, false, true))
		{
			++queued;
			if (g_counts_ready && HasAnyRule(g_classes[class_index].rule))
				AdjustCounts(class_index, -1);
		}
		// Young and pack members following it go too, or they would be left wandering where it was.
		UClass* dino_class = APrimalDinoCharacter::GetPrivateStaticClass();
		auto& followers = dino->WildFollowerRefsField();
		for (int32 f = 0; f < followers.Num(); ++f)
		{
			AActor* follower = followers[f];
			if (follower == nullptr || !follower->IsA(dino_class))
				continue;
			APrimalDinoCharacter* follower_dino = static_cast<APrimalDinoCharacter*>(follower);
			if (Classify(follower_dino) != Kind::Wild)
				continue;
			const int follower_class = ClassIndexFor(follower_dino->ClassField());
			if (g_classes[follower_class].rule.is_protected)
				continue;
			if (Enqueue(follower_dino, follower_class, false, true))
			{
				++queued;
				if (g_counts_ready && HasAnyRule(g_classes[follower_class].rule))
					AdjustCounts(follower_class, -1);
			}
		}
		if (queued == 0)
			return;
		g_mesh_removal_queued += queued;
		g_mesh_removal_points.push_back({ static_cast<double>(location.X), static_cast<double>(location.Y), static_cast<double>(location.Z), class_index });
		if (g_mesh_removal_points.size() > kRemovalPointsKept)
			g_mesh_removal_points.pop_front();
		if (!g_settings.log_removals)
			return;
		if (g_mesh_logged_this_minute >= kMeshLogsPerMinute)
		{
			++g_mesh_unlogged_this_minute;
			return;
		}
		++g_mesh_logged_this_minute;
		Log::GetLog()->info("Under mesh: removing {} at {:.0f} {:.0f} {:.0f} (below on two censuses){}", g_classes[class_index].info.short_name,
			static_cast<double>(location.X), static_cast<double>(location.Y), static_cast<double>(location.Z),
			queued > 1 ? " with " + std::to_string(queued - 1) + " follower(s)" : std::string());
	}

	void MeshTick()
	{
		if (g_rules == nullptr || !g_rules->under_mesh.enabled || g_mesh_queue.empty())
			return;
		int budget = g_rules->under_mesh.checks_per_second;
		size_t pops = 0;
		while (budget > 0 && pops < kMeshPopsPerSecond && !g_mesh_queue.empty())
		{
			MeshCandidate candidate = g_mesh_queue.front();
			g_mesh_queue.pop_front();
			++pops;
			APrimalDinoCharacter* dino = candidate.creature.Get();
			if (dino == nullptr || Classify(dino) != Kind::Wild)
				continue; // gone or tamed
			const bool in_stasis = dino->bStasised()();
			if (in_stasis && !g_rules->under_mesh.include_stasis)
				continue; // asleep in stasis: tested when a later census finds it awake
			const uint64_t key = WeakKey(candidate.creature);
			const bool removing = g_rules->under_mesh.remove;
			if (g_mesh_reported.count(key) > 0 && !removing)
				continue; // reported once is enough when nothing is removed; with Remove it is tested again to confirm
			FVector location = dino->GetLocation();
				// Places the mapmaker marked as allowed below the surface (caves and the like).
				if (UVictoryCore::IsBoundsInUndermeshExclusionZone(World(), &location, 200.0f))
				{
					++g_mesh_in_exclusion_zone;
					continue;
				}

			FVector hit_location(0, 0, 0);
			bool overlapping = false;
			UActorComponent* hit = nullptr;
			const auto start = Clock::now();
			const bool under = UVictoryCore::IsUnderMesh(dino, &hit_location, &overlapping, &hit, false, 0.0f);
			if (!under && !g_creature_zone.empty())
				NoteMeshZoneClear(key);
			const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
			g_mesh_total_us += us;
			g_mesh_max_us = (std::max)(g_mesh_max_us, us);
			++g_mesh_checks;
			--budget;

			bool confirmed = false;
			if (removing)
			{
				uint32_t first = 0;
				const auto seen = g_mesh_first_below.find(key);
				if (seen != g_mesh_first_below.end())
					first = seen->second;
				confirmed = ConfirmUnderMesh(under, overlapping, g_census_number, first);
				if (first == 0 || confirmed)
					g_mesh_first_below.erase(key);
				else
					g_mesh_first_below[key] = first;
			}
			if (confirmed)
			{
				RemoveUnderMesh(dino, candidate.class_index, location);
				NoteMeshZoneRemoval(key, location);
			}
			if (!under || !g_mesh_reported.insert(key).second)
				continue;

			++g_mesh_suspects;
			++g_mesh_suspects_by_class[candidate.class_index];
			MeshSuspect suspect;
			suspect.class_index = candidate.class_index;
			suspect.x = static_cast<double>(location.X);
			suspect.y = static_cast<double>(location.Y);
			suspect.z = static_cast<double>(location.Z);
			suspect.overlapping = overlapping;
			suspect.in_stasis = in_stasis;
			UObject* hit_object = reinterpret_cast<UObject*>(hit);
			suspect.hit = hit_object != nullptr && hit_object->ClassField() != nullptr ? hit_object->ClassField()->NameField().ToString().ToString() : "nothing";
			g_mesh_recent.push_front(suspect);
			if (g_mesh_recent.size() > kMeshRecent)
				g_mesh_recent.pop_back();
			// While removing, the removal line says the same thing a census later.
			if (g_settings.log_removals && !g_rules->under_mesh.remove && g_mesh_logged_this_minute < kMeshLogsPerMinute)
			{
				++g_mesh_logged_this_minute;
				Log::GetLog()->info("Under mesh: {} at {:.0f} {:.0f} {:.0f} ({}{}, hit {})", g_classes[candidate.class_index].info.short_name,
					suspect.x, suspect.y, suspect.z, overlapping ? "overlapping" : "below", in_stasis ? ", in stasis" : "", suspect.hit);
			}
		}
	}

	void SafeTimer()
	{
		try
		{
			OnTimer();
		}
		catch (const std::exception& exception)
		{
			Log::GetLog()->error("Timer failed: {}", exception.what());
		}
	}

	// ------------------------------------------------------------------------------------------
	// Commands
	// ------------------------------------------------------------------------------------------

	std::string ListText(const std::string& filter)
	{
		std::ostringstream out;
		out << "SpawnControl on " << (g_map_name.empty() ? std::string("?") : g_map_name) << ": section "
			<< (g_rules != nullptr ? g_rules->name : std::string("none")) << " (" << g_section_source << ")\n";

		if (!g_have_report)
		{
			out << "First census still running.\n";
			return out.str();
		}

		const double age = std::chrono::duration<double>(Clock::now() - g_last.finished).count();
		out << "Census " << static_cast<int>(age) << " s ago (" << std::fixed;
		out.precision(1);
		out << g_last.seconds << " s, " << g_last.actors << " actors, planned in " << g_last.plan_ms << " ms): " << g_last.wild << " wild, "
			<< g_last.mission << " mission-owned";
		if (g_last.summoned > 0)
			out << ", " << g_last.summoned << " of the wild summoned by another creature (not limited)";
		out << "\n";
		out << "Queue " << g_queue.size() << ", removed " << g_removed_total << " (skipped " << g_removed_skipped
			<< "), spawn check avg " << (g_hook_calls > 0 ? g_hook_total_us / g_hook_calls : 0.0) << " us, max "
			<< g_hook_max_us << " us\n";
		out << "Per tick: census slice avg " << g_census_timing.Average() << " ms, max " << g_census_timing.max_ms
			<< " ms; removals avg " << g_drain_timing.Average() << " ms, max " << g_drain_timing.max_ms << " ms ("
			<< g_drain_timing.ticks << " ticks)\n";
		if (g_settings.refresh_hours > 0)
			out << "Rolling refresh: turnover " << g_settings.refresh_hours << " h, " << g_refreshed_total << " refreshed\n";
		out << "Zone entries: " << g_entry_checks << " checked, " << g_entry_refused << " refused, avg "
			<< (g_entry_checks > 0 ? g_entry_total_us / g_entry_checks : 0.0) << " us, max " << g_entry_max_us << " us"
			<< (g_entry_hook_installed ? "" : " (hook not installed)")
			<< (g_settings.refuse_full_entries ? "" : " (RefuseFullEntries off)") << "; " << g_entry_unloaded_class
			<< " left to the game (a class not loaded through that entry)\n";
		{
			size_t holding = 0, ever = 0;
			for (const auto& [zone, churn] : g_zone_churn)
			{
				holding += churn.holding ? 1 : 0;
				ever += churn.holds > 0 ? 1 : 0;
			}
			out << "Churning zones (spawns over a limit, removed on arrival): " << holding << " held back now, " << ever
				<< " since reload, " << g_churn_held_calls << " spawn passes skipped\n";
			size_t shown = 0;
			for (const auto& [zone, churn] : g_zone_churn)
			{
				if (!churn.holding || shown >= 10)
					continue;
				++shown;
				std::string names;
				for (int c : churn.classes)
					names += (names.empty() ? "" : ", ") + g_classes[c].info.short_name;
				out << "  held: " << ObjectName(zone) << " (" << names << "; held " << churn.holds << " time(s); last at "
					<< static_cast<int>(churn.x) << " " << static_cast<int>(churn.y) << " " << static_cast<int>(churn.z) << ")\n";
			}
		}
		out << "Summoned (appeared outside a zone spawn): " << g_summoned.size() << " tracked, " << g_summoned_seen
			<< " since load; zone spawn calls " << g_zone_spawn_calls
			<< (g_zone_hook_installed ? "" : " (hook not installed)")
			<< (g_settings.ignore_summoned ? "" : " (IgnoreSummoned off)") << "\n";
		{
			// Which classes the last census found marked summoned, largest first.
			std::vector<std::pair<int, size_t>> rows;
			for (size_t c = 0; c < g_last.summoned_by_class.size() && c < g_classes.size(); ++c)
			{
				if (g_last.summoned_by_class[c] > 0)
					rows.emplace_back(g_last.summoned_by_class[c], c);
			}
			std::sort(rows.rbegin(), rows.rend());
			if (!rows.empty())
			{
				out << "  summoned at the last census:";
				for (size_t i = 0; i < rows.size() && i < 8; ++i)
					out << (i == 0 ? " " : ", ") << rows[i].first << " " << g_classes[rows[i].second].info.short_name;
				out << (rows.size() > 8 ? ", ...\n" : "\n");
			}
		}

		if (!filter.empty())
		{
			// Every class whose name or path contains the filter, with its rule.
			const std::string needle = ToLower(filter);
			for (size_t c = 0; c < g_classes.size() && c < g_last.wild_by_class.size(); ++c)
			{
				const ClassEntry& entry = g_classes[c];
				if (ToLower(entry.info.path).find(needle) == std::string::npos)
					continue;
				out << "  " << entry.info.short_name << ": " << g_last.wild_by_class[c] << " wild";
				if (g_last.mission_by_class[c] > 0)
					out << ", " << g_last.mission_by_class[c] << " mission";
				if (g_last.summoned_by_class[c] > 0)
					out << " (" << g_last.summoned_by_class[c] << " of them summoned)";
				if (entry.rule.is_protected)
					out << " [protected]";
				if (entry.rule.blocked)
					out << " [blocked]";
				if (entry.rule.limit != kNoLimit)
					out << " [limit " << entry.rule.limit << "]";
				out << "\n    " << entry.info.path << "\n";
				// Parent classes up to the base creature class, for Exclude and Protect.Families entries.
				std::string parents;
				for (size_t p = 1; p < entry.info.chain.size(); ++p)
				{
					parents += (parents.empty() ? "" : " > ") + entry.info.chain[p];
					if (entry.info.chain[p] == "PrimalDinoCharacter")
						break;
				}
				if (!parents.empty())
					out << "    parents: " << parents << "\n";
			}
			return out.str();
		}

		// Ruled classes and groups show the live count: the last census, minus what it queued for removal,
		// plus spawns since. A census count over a limit would only show what is already on its way out.
		const bool live = g_counts_ready;
		auto count_now = [&](size_t c) { return live && c < g_class_counts.size() ? g_class_counts[c] : g_last.wild_by_class[c]; };
		out << (live ? "Ruled classes (now, after queued removals):\n" : "Ruled classes (last census):\n");
		for (size_t c = 0; c < g_classes.size() && c < g_last.wild_by_class.size(); ++c)
		{
			const ClassRule& rule = g_classes[c].rule;
			if (rule.blocked && count_now(c) > 0)
				out << "  " << g_classes[c].info.short_name << ": " << count_now(c) << " [blocked]\n";
			else if (rule.limit != kNoLimit)
				out << "  " << g_classes[c].info.short_name << ": " << count_now(c) << " / " << rule.limit << "\n";
		}
		if (g_rules != nullptr)
		{
			for (size_t g = 0; g < g_rules->groups.size(); ++g)
			{
				int total = 0;
				for (size_t c = 0; c < g_classes.size() && c < g_last.wild_by_class.size(); ++c)
				{
					const auto& member_of = g_classes[c].rule.groups;
					if (std::find(member_of.begin(), member_of.end(), static_cast<int>(g)) != member_of.end())
						total += count_now(c);
				}
				out << "  group " << g_rules->groups[g].name << ": " << total << " / " << g_rules->groups[g].max;
				std::string members;
				for (size_t c = 0; c < g_classes.size() && c < g_last.wild_by_class.size(); ++c)
				{
					const auto& member_of = g_classes[c].rule.groups;
					if (std::find(member_of.begin(), member_of.end(), static_cast<int>(g)) != member_of.end())
						members += (members.empty() ? "" : ", ") + g_classes[c].info.short_name + " " + std::to_string(count_now(c));
				}
				out << " (" << members << ")\n";
			}
			for (size_t d = 0; d < g_rules->density.size(); ++d)
			{
				const DensityRule& rule = g_rules->density[d];
				out << "  density " << rule.name << ": max " << rule.max << " per " << static_cast<int>(rule.size / kFoundation)
					<< " foundations" << (rule.exclude.empty() ? "" : ", with exclusions") << "\n";
				if (d < g_last.hotspots.size() && !g_last.hotspots[d].empty())
				{
					out << "    fullest cubes last census (before thinning):\n";
					for (const DensityHotspot& spot : g_last.hotspots[d])
					{
						out << "      " << spot.count << " at " << static_cast<int>(spot.x) << " " << static_cast<int>(spot.y) << " "
							<< static_cast<int>(spot.z) << " (";
						for (size_t k = 0; k < spot.classes.size() && k < 4; ++k)
						{
							const int c = spot.classes[k].second;
							out << (k == 0 ? "" : ", ") << spot.classes[k].first << " "
								<< (c >= 0 && static_cast<size_t>(c) < g_classes.size() ? g_classes[c].info.short_name : std::string("?"));
						}
						out << (spot.classes.size() > 4 ? ", ...)" : ")") << "\n";
					}
				}
			}
			if (!g_rules->density.empty())
			{
				out << "  density skips: caves " << (g_rules->density_skip.caves ? "on" : "off") << "";
				for (const DensityArea& area : g_rules->density_skip.areas)
					out << ", " << (area.name.empty() ? std::string("area") : area.name) << " (" << static_cast<int>(area.radius / kFoundation)
						<< " foundations)";
				out << "\n  density removals planned by the last census: " << g_last.too_dense << "\n";
			}
			for (const BlockArea& area : g_rules->block_areas)
			{
				out << "  block area " << (area.name.empty() ? std::string("(unnamed)") : area.name) << ": "
					<< static_cast<int>(area.radius / kFoundation) << " foundations around " << static_cast<int>(area.x) << " "
					<< static_cast<int>(area.y);
				if (area.has_z)
					out << " " << static_cast<int>(area.z) << " (sphere)";
				else
					out << " (any height)";
				out << "\n";
			}
			if (!g_rules->block_areas.empty())
			{
				out << "  block areas: " << g_block_area_queued << " queued since load, " << g_last.in_block_area
					<< " inside at the last census; zones stopped " << g_zones_stopped << " (" << g_stopped_zone_refusals
					<< " entries refused)\n";
				for (const auto& [zone, stats] : g_zone_stats)
				{
					if (stats.stopped && !stats.by_mesh)
						out << "    stopped: " << ObjectName(zone) << " (" << stats.inside << " spawned into " << stats.area << "; last at "
							<< static_cast<int>(stats.x) << " " << static_cast<int>(stats.y) << " " << static_cast<int>(stats.z) << ")\n";
				}
			}
			if (g_rules->under_mesh.enabled)
			{
				out << "  under mesh (" << (g_rules->under_mesh.remove ? "removing: below on two censuses" : "report only")
					<< (g_rules->under_mesh.include_stasis ? ", stasis included" : ", awake only") << "): "
					<< g_mesh_checks << " tested, avg "
					<< (g_mesh_checks > 0 ? g_mesh_total_us / g_mesh_checks : 0.0) << " us, max " << g_mesh_max_us << " us, "
					<< g_mesh_suspects << " suspects since load";
				out << ", " << g_mesh_in_exclusion_zone << " skipped in the map's exclusion zones";
				out << "; " << g_mesh_queue.size() << " waiting for this census";
				if (g_rules->under_mesh.remove)
					out << "; " << g_mesh_removal_queued << " queued for removal since load, " << g_mesh_first_below.size()
						<< " found below once, waiting for a second census";
				out << "\n";
				if (MeshZoneStopping())
				{
					size_t tracked_zones = 0;
					for (const auto& [zone, stats] : g_zone_stats)
						tracked_zones += stats.under > 0 ? 1 : 0;
					out << "    zones stopped for spawning under the mesh: " << g_mesh_zones_stopped << " (stop after "
						<< kMeshZoneStopAfter << " removals with at most 1 clear per " << kMeshZoneClearRatio << "; " << tracked_zones
						<< " zones with removals since reload, " << g_creature_zone.size() << " creatures traced to their zone)\n";
					for (const auto& [zone, stats] : g_zone_stats)
					{
						if (stats.stopped && stats.by_mesh)
						{
							out << "      stopped: " << ObjectName(zone) << " (" << stats.under << " removed, " << stats.clear
								<< " clear; last at " << static_cast<int>(stats.x) << " " << static_cast<int>(stats.y) << " "
								<< static_cast<int>(stats.z) << ")\n";
						}
					}
				}
				if (!g_mesh_suspects_by_class.empty())
				{
					std::vector<std::pair<uint32_t, int>> by_class;
					for (const auto& [c, n] : g_mesh_suspects_by_class)
						by_class.emplace_back(n, c);
					std::sort(by_class.rbegin(), by_class.rend());
					out << "    suspects by class:";
					for (size_t i = 0; i < by_class.size() && i < 10; ++i)
					{
						const int c = by_class[i].second;
						out << (i == 0 ? " " : ", ") << by_class[i].first << " "
							<< (c >= 0 && static_cast<size_t>(c) < g_classes.size() ? g_classes[c].info.short_name : std::string("?"));
					}
					out << (by_class.size() > 10 ? ", ...\n" : "\n");
				}
				// Spots where removals keep happening: a spawn point inside the terrain. Each comes with a BlockAreas
				// line to paste, so the zone is stopped instead of feeding the queue.
				const std::vector<RemovalPoint> points(g_mesh_removal_points.begin(), g_mesh_removal_points.end());
				const std::vector<RepeatSpot> spots = FindRepeatSpots(points, kRepeatSpotReach, kRepeatSpotMinimum, g_rules->block_areas, 10);
				if (!spots.empty())
				{
					out << "    repeat spots (" << kRepeatSpotMinimum << "+ removals within " << static_cast<int>(kRepeatSpotReach / kFoundation)
						<< " foundations, not yet in a block area); check each, then paste into BlockAreas:\n";
					for (size_t i = 0; i < spots.size(); ++i)
					{
						const RepeatSpot& spot = spots[i];
						std::string classes;
						for (size_t k = 0; k < spot.classes.size() && k < 3; ++k)
						{
							const int c = spot.classes[k].second;
							classes += (k == 0 ? "" : ", ") + std::to_string(spot.classes[k].first) + " " +
								(c >= 0 && static_cast<size_t>(c) < g_classes.size() ? g_classes[c].info.short_name : std::string("?"));
						}
						const std::string first_class = !spot.classes.empty() && spot.classes[0].second >= 0 &&
							static_cast<size_t>(spot.classes[0].second) < g_classes.size() ? g_classes[spot.classes[0].second].info.short_name : "spot";
						out << "      " << spot.count << " removed (" << classes << "):\n"
							<< "        { \"Name\": \"Repeat " << (i + 1) << " " << first_class << "\", \"Location\": [ "
							<< static_cast<int>(spot.x) << ", " << static_cast<int>(spot.y) << ", " << static_cast<int>(spot.z)
							<< " ], \"RadiusInFoundations\": " << spot.radius_foundations << " }\n";
					}
				}
				for (const MeshSuspect& suspect : g_mesh_recent)
				{
					const int c = suspect.class_index;
					out << "    " << (c >= 0 && static_cast<size_t>(c) < g_classes.size() ? g_classes[c].info.short_name : std::string("?"))
						<< " at " << static_cast<int>(suspect.x) << " " << static_cast<int>(suspect.y) << " " << static_cast<int>(suspect.z)
						<< " (" << (suspect.overlapping ? "overlapping" : "below") << (suspect.in_stasis ? ", in stasis" : "") << ", hit " << suspect.hit << ")\n";
				}
			}
			if (g_rules->overlap.enabled)
			{
				out << "  overlap: inside another creature when closer than " << g_rules->overlap.fraction
					<< " of their summed radii" << (g_rules->overlap.exclude.empty() ? "" : ", with exclusions")
					<< "; " << g_last.overlapping << " found by the last census\n";
				auto name = [](int c) { return c >= 0 && static_cast<size_t>(c) < g_classes.size() ? g_classes[c].info.short_name : std::string("?"); };
				for (const OverlapSample& sample : g_last.overlap_samples)
					out << "    " << name(sample.removed_class) << " inside " << name(sample.kept_class) << " at "
						<< static_cast<int>(sample.x) << " " << static_cast<int>(sample.y) << " " << static_cast<int>(sample.z) << "\n";
			}
		}
		for (size_t f = 0; f < g_settings.report_families.size() && f < g_last.family_wild.size(); ++f)
		{
			out << "  " << g_settings.report_families[f].label << ": " << g_last.family_wild[f] << " without a mission, "
				<< g_last.family_mission[f] << " mission-owned\n";
		}

		// The ten largest classes without any rule: candidates for a limit.
		std::vector<std::pair<int, size_t>> unruled;
		for (size_t c = 0; c < g_classes.size() && c < g_last.wild_by_class.size(); ++c)
		{
			if (!HasAnyRule(g_classes[c].rule) && !g_classes[c].rule.is_protected && g_last.wild_by_class[c] > 0)
				unruled.emplace_back(g_last.wild_by_class[c], c);
		}
		std::sort(unruled.begin(), unruled.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		if (!unruled.empty())
			out << "Largest without a rule:\n";
		for (size_t i = 0; i < unruled.size() && i < 10; ++i)
			out << "  " << g_classes[unruled[i].second].info.short_name << ": " << unruled[i].first << "\n";
		return out.str();
	}

	// Rules: what this server is actually running with, each map value marked as its own or from Default.
	std::string RulesText()
	{
		std::ostringstream out;
		out << "SpawnControl rules: section " << (g_rules != nullptr ? g_rules->name : std::string("none")) << " for "
			<< g_section_source << "\n";
		out << "Read from: " << SourcesText() << "\n";
		out << "General: RemovalsPerTick " << g_settings.removals_per_tick << ", CensusIntervalSeconds " << g_settings.census_interval_seconds
			<< ", CensusActorsPerTick " << g_settings.census_actors_per_tick << ", NearPlayerDistance " << g_settings.near_player_distance
			<< ", LogRemovals " << (g_settings.log_removals ? "true" : "false") << ", RefuseFullEntries "
			<< (g_settings.refuse_full_entries ? "true" : "false") << ", IgnoreSummoned " << (g_settings.ignore_summoned ? "true" : "false") << "\n";
		if (g_rules == nullptr)
		{
			out << "No section for this map and no Default section, so nothing is ruled here\n";
			return out.str();
		}
		if (!g_settings.has_default)
			out << "No Default section; every value below is this section's own\n";
		for (const std::string& line : DescribeRules(*g_rules))
			out << "  " << line << "\n";
		if (!g_settings.warnings.empty())
			out << g_settings.warnings.size() << " config warning(s) at the last load; they are in the log\n";
		return out.str();
	}

	std::string ReloadCommandText()
	{
		std::string error;
		if (!ReloadSettings(error))
		{
			Log::GetLog()->error("Config reload failed: {}", error);
			return "SpawnControl reload failed, old settings kept: " + error;
		}
		std::string text = "SpawnControl reloaded: section " + (g_rules != nullptr ? g_rules->name : std::string("none")) +
			" for " + g_section_source;
		if (!g_settings.warnings.empty())
			text += "; " + std::to_string(g_settings.warnings.size()) + " warning(s) in the log";
		return text;
	}

	std::string CommandArgument(const FString* command)
	{
		if (command == nullptr)
			return {};
		std::istringstream words(command->ToString());
		std::string word;
		words >> word; // the command itself
		std::string rest;
		std::getline(words, rest);
		const size_t start = rest.find_first_not_of(' ');
		return start == std::string::npos ? std::string() : rest.substr(start);
	}

	void SendAdmin(AShooterPlayerController* player, const std::string& text, bool ok = true)
	{
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			const std::wstring wide = AsaApi::Tools::Utf8Decode(line);
			AsaApi::GetApiUtils().SendServerMessage(player, ok ? FColorList::Green : FColorList::Red, L"{}", wide.c_str());
		}
	}

	AShooterPlayerController* AdminFrom(APlayerController* controller)
	{
		auto* player = static_cast<AShooterPlayerController*>(controller);
		if (player == nullptr || !player->bIsAdmin()())
			return nullptr;
		return player;
	}

	// MeshHere: the game's under-mesh test on every creature near the admin, right now, whatever its state.
	std::string MeshHereText(const FVector& at, int radius_foundations)
	{
		UWorld* world = World();
		if (world == nullptr)
			return "No world.\n";
		const double radius = radius_foundations * kFoundation;
		struct Row
		{
			double distance = 0;
			bool under = false;
			std::string text;
		};
		size_t under_count = 0;
		std::vector<Row> rows;
		UClass* dino_class = APrimalDinoCharacter::GetPrivateStaticClass();
		auto& levels = world->LevelsField();
		for (int32 l = 0; l < levels.Num(); ++l)
		{
			ULevel* level = levels[l].Get();
			if (level == nullptr)
				continue;
			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			for (int a = 0; a < actors.Num(); ++a)
			{
				AActor* actor = actors[a];
				if (actor == nullptr || !actor->IsA(dino_class))
					continue;
				APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
				FVector location = dino->GetLocation();
				const double distance = std::sqrt(DistanceSquared(location, at));
				if (distance > radius)
					continue;

			const bool excluded = UVictoryCore::IsBoundsInUndermeshExclusionZone(World(), &location, 200.0f);
				FVector hit_location(0, 0, 0);
				bool overlapping = false;
				UActorComponent* hit = nullptr;
				const bool under = UVictoryCore::IsUnderMesh(dino, &hit_location, &overlapping, &hit, false, 0.0f);
				UObject* hit_object = reinterpret_cast<UObject*>(hit);
				const std::string hit_name = hit_object != nullptr && hit_object->ClassField() != nullptr ?
					hit_object->ClassField()->NameField().ToString().ToString() : "nothing";
				const Kind kind = Classify(dino);
				const char* kind_name = kind == Kind::Wild ? "wild" : kind == Kind::Mission ? "mission" : kind == Kind::Summoned ? "summoned" : "tamed/dead";
				const int class_index = ClassIndexFor(dino->ClassField());
				std::ostringstream line;
				line << g_classes[class_index].info.short_name << " " << static_cast<int>(distance / kFoundation) << " fdn, "
					<< static_cast<int>((static_cast<double>(location.Z) - static_cast<double>(at.Z)) / kFoundation) << " fdn up/down: "
					<< kind_name << (dino->bStasised()() ? ", in stasis" : ", awake") << " -> "
					<< (under ? (overlapping ? "UNDER (overlapping)" : "UNDER (below)") : "clear") << ", hit " << hit_name
					<< (excluded ? " [map exclusion zone]" : "")
					<< (g_mesh_reported.count(WeakKey(GetWeakReference(dino))) > 0 ? " [already reported]" : "");
				rows.push_back({ distance, under, line.str() });
				if (under)
					++under_count;
			}
		}
		// Under first, then nearest first, so a long list never hides a hit.
		std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.under != y.under ? x.under : x.distance < y.distance; });
		std::ostringstream out;
		out << "Under-mesh test on " << rows.size() << " creature(s) within " << radius_foundations << " foundations of "
			<< static_cast<int>(at.X) << " " << static_cast<int>(at.Y) << " " << static_cast<int>(at.Z) << ": "
			<< (rows.size() - under_count) << " clear, " << under_count << " under\n";
		for (size_t i = 0; i < rows.size() && i < 40; ++i)
			out << "  " << rows[i].text << "\n";
		if (rows.size() > 40)
			out << "  ... " << (rows.size() - 40) << " more\n";
		return out.str();
	}

	void ConsoleMeshHere(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			int radius = 30;
			const std::string argument = CommandArgument(command);
			if (!argument.empty())
			{
				try
				{
					radius = (std::max)(1, (std::min)(std::stoi(argument), 200));
				}
				catch (const std::exception&)
				{
				}
			}
			const std::string text = MeshHereText(AsaApi::IApiUtils::GetPosition(admin), radius);
			Log::GetLog()->info("MeshHere:\n{}", text);
			SendAdmin(admin, text);
		}
	}

	void ConsoleReload(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = ReloadCommandText();
			SendAdmin(admin, text, text.find("failed") == std::string::npos);
		}
	}

	void ConsoleList(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = ListText(CommandArgument(command));
			Log::GetLog()->info("List:\n{}", text);
			SendAdmin(admin, text);
		}
	}

	// ------------------------------------------------------------------------------------------
	// Links: what the wild creatures of matching classes are linked to (owner, instigator, wild
	// follower parent and followers, zone manager link). One pass over every level actor.
	// ------------------------------------------------------------------------------------------

	std::string ClassNameOf(UObject* object)
	{
		if (object == nullptr)
			return "none";
		UClass* cls = object->ClassField();
		return cls != nullptr ? cls->NameField().ToString().ToString() : "?";
	}

	std::string LinksText(const std::string& filter)
	{
		if (filter.empty())
			return std::string("Usage: ") + kCmdLinks + " <text>, e.g. " + kCmdLinks + " skinny\n";
		UWorld* world = World();
		if (world == nullptr)
			return "No world.\n";

		using Tally = std::map<std::string, int>;
		Tally owner, instigator, parent, followers_of_leaders, matched_classes;
		int total = 0, self_owner = 0, self_instigator = 0, direct_link = 0, follower_bit = 0, leaders = 0, summoned = 0;

		const std::string needle = ToLower(filter);
		UClass* dino_class = APrimalDinoCharacter::GetPrivateStaticClass();
		auto& levels = world->LevelsField();
		for (int32 l = 0; l < levels.Num(); ++l)
		{
			ULevel* level = levels[l].Get();
			if (level == nullptr)
				continue;
			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			for (int32 a = 0; a < actors.Num(); ++a)
			{
				AActor* actor = actors[a];
				if (actor == nullptr || !actor->IsA(dino_class))
					continue;
				APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
				const Kind kind = Classify(dino);
				if (kind != Kind::Wild && kind != Kind::Summoned)
					continue;
				const ClassEntry& entry = g_classes[ClassIndexFor(dino->ClassField())];
				if (ToLower(entry.info.path).find(needle) == std::string::npos)
					continue;

				++total;
				++matched_classes[entry.info.short_name];
				if (kind == Kind::Summoned)
					++summoned;
				AActor* own = dino->OwnerField().Get();
				AActor* ins = dino->InstigatorField().Get();
				if (own == dino) ++self_owner; else ++owner[ClassNameOf(own)];
				if (ins == dino) ++self_instigator; else ++instigator[ClassNameOf(ins)];
				++parent[ClassNameOf(dino->WildFollowingParentRefField())];
				if (dino->DirectLinkNPCZoneManagerField() != nullptr)
					++direct_link;
				if (dino->bIsAWildFollowerKnownServerside()())
					++follower_bit;
				auto& followers = dino->WildFollowerRefsField();
				if (followers.Num() > 0)
				{
					++leaders;
					for (int32 f = 0; f < followers.Num(); ++f)
						++followers_of_leaders[ClassNameOf(followers[f])];
				}
			}
		}

		std::ostringstream out;
		auto write = [&out](const char* label, const Tally& tally)
		{
			std::vector<std::pair<int, std::string>> rows;
			for (const auto& [name, count] : tally)
				rows.emplace_back(count, name);
			std::sort(rows.rbegin(), rows.rend());
			out << label << ":";
			if (rows.empty())
				out << " -";
			for (size_t i = 0; i < rows.size() && i < 8; ++i)
				out << (i == 0 ? " " : ", ") << rows[i].second << " " << rows[i].first;
			out << "\n";
		};
		out << "Links of " << total << " wild creature(s) matching \"" << filter << "\" (" << summoned
			<< " counted as summoned)\n";
		write("Classes", matched_classes);
		out << "Owner: self " << self_owner << "; ";
		write("others", owner);
		out << "Instigator: self " << self_instigator << "; ";
		write("others", instigator);
		write("Wild following parent", parent);
		out << "Wild follower flag set: " << follower_bit << "; zone manager direct link: " << direct_link << "\n";
		out << "Leading followers: " << leaders << "; ";
		write("their followers", followers_of_leaders);
		return out.str();
	}

	void ConsoleLinks(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = LinksText(CommandArgument(command));
			Log::GetLog()->info("Links:\n{}", text);
			SendAdmin(admin, text);
		}
	}

	void RconReply(RCONClientConnection* connection, RCONPacket* packet, const std::string& text)
	{
		FString reply(text);
		connection->SendMessageW(packet->Id, 0, &reply);
	}

	void RconReload(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		RconReply(connection, packet, ReloadCommandText());
	}

	void RconList(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		RconReply(connection, packet, ListText(CommandArgument(&packet->Body)));
	}

	void ConsoleRules(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = RulesText();
			Log::GetLog()->info("Rules:\n{}", text);
			SendAdmin(admin, text);
		}
	}

	void RconRules(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		const std::string text = RulesText();
		Log::GetLog()->info("Rules:\n{}", text);
		RconReply(connection, packet, text);
	}

	void RconLinks(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		const std::string text = LinksText(CommandArgument(&packet->Body));
		Log::GetLog()->info("Links:\n{}", text);
		RconReply(connection, packet, text);
	}
} // namespace

// Called by AsaApi when the plugin is loaded.
extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(PROJECT_NAME);

	std::string error;
	if (!ReloadSettings(error))
	{
		Log::GetLog()->error("Config not loaded: {}", error);
		Log::GetLog()->error("Nothing is limited until config.json is fixed and {} is run", kCmdReload);
	}

	g_entry_hook_installed = AsaApi::GetHooks().SetHook(kHookEntry,
		reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_IsValidNPCEntry), &ANPCZoneManager_IsValidNPCEntry_original);
	if (!g_entry_hook_installed)
		Log::GetLog()->error("Could not hook {}; zones will keep spawning full classes for the queue to remove", kHookEntry);

	g_hook_installed = AsaApi::GetHooks().SetHook(kHookBeginPlay,
		reinterpret_cast<LPVOID>(&Hook_APrimalDinoCharacter_BeginPlay), &APrimalDinoCharacter_BeginPlay_original);
	if (!g_hook_installed)
		Log::GetLog()->error("Could not hook {}; only the census will enforce limits", kHookBeginPlay);

	g_zone_hook_installed = AsaApi::GetHooks().SetHook(kHookZoneSpawn,
		reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_SpawnNPCs), &ANPCZoneManager_SpawnNPCs_original);
	if (!g_zone_hook_installed)
		Log::GetLog()->error("Could not hook {}; summoned creatures cannot be told apart and are limited like the rest",
			kHookZoneSpawn);

	AsaApi::GetCommands().AddConsoleCommand(kCmdReload, &ConsoleReload);
	AsaApi::GetCommands().AddConsoleCommand(kCmdList, &ConsoleList);
	AsaApi::GetCommands().AddConsoleCommand(kCmdMeshHere, &ConsoleMeshHere);
	AsaApi::GetCommands().AddConsoleCommand(kCmdLinks, &ConsoleLinks);
	AsaApi::GetCommands().AddConsoleCommand(kCmdRules, &ConsoleRules);
	AsaApi::GetCommands().AddRconCommand(kCmdReload, &RconReload);
	AsaApi::GetCommands().AddRconCommand(kCmdList, &RconList);
	AsaApi::GetCommands().AddRconCommand(kCmdLinks, &RconLinks);
	AsaApi::GetCommands().AddRconCommand(kCmdRules, &RconRules);
	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &SafeTimer);
}

// Called by AsaApi when the plugin is unloaded.
extern "C" __declspec(dllexport) void Plugin_Unload()
{
	AsaApi::GetCommands().RemoveOnTimerCallback(kTimerId);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdReload);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdList);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdMeshHere);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdLinks);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdRules);
	AsaApi::GetCommands().RemoveRconCommand(kCmdReload);
	AsaApi::GetCommands().RemoveRconCommand(kCmdList);
	AsaApi::GetCommands().RemoveRconCommand(kCmdLinks);
	AsaApi::GetCommands().RemoveRconCommand(kCmdRules);

	if (g_entry_hook_installed)
	{
		AsaApi::GetHooks().DisableHook(kHookEntry, reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_IsValidNPCEntry));
		g_entry_hook_installed = false;
	}
	if (g_hook_installed)
	{
		AsaApi::GetHooks().DisableHook(kHookBeginPlay, reinterpret_cast<LPVOID>(&Hook_APrimalDinoCharacter_BeginPlay));
		g_hook_installed = false;
	}
	if (g_zone_hook_installed)
	{
		AsaApi::GetHooks().DisableHook(kHookZoneSpawn, reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_SpawnNPCs));
		g_zone_hook_installed = false;
	}
}
