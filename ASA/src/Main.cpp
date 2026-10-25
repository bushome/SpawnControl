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
#include <deque>
#include <fstream>
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
	const char* const kTimerId = "SpawnControl.Timer";
	const char* const kHookEntry = "ANPCZoneManager.IsValidNPCEntry(FNPCSpawnEntry&)";
	const char* const kHookBeginPlay = "APrimalDinoCharacter.BeginPlay()";
	const char* const kLevelActorsField = "ULevel.Actors2";
	const wchar_t* const kMapSwitch = L"-SpawnControlMap=";

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
	};

	struct CensusItem
	{
		TWeakObjectPtr<APrimalDinoCharacter> creature;
		int class_index = 0;
		double player_distance = 0;
	};

	struct Report
	{
		Clock::time_point finished{};
		double seconds = 0;
		size_t actors = 0;
		size_t wild = 0;
		size_t mission = 0;
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
		Wild,
	};

	Kind Classify(APrimalDinoCharacter* dino)
	{
		if (dino == nullptr || dino->IsDead())
			return Kind::Other;
		if (dino->TargetingTeamField() >= kFirstPlayerTeam)
			return Kind::Other;
		if (dino->OwnerMissionField() != nullptr)
			return Kind::Mission;
		return Kind::Wild;
	}

	// ------------------------------------------------------------------------------------------
	// Config
	// ------------------------------------------------------------------------------------------

	std::string MapSwitchValue()
	{
		const std::wstring line = GetCommandLineW();
		const size_t at = line.find(kMapSwitch);
		if (at == std::wstring::npos)
			return {};
		size_t start = at + wcslen(kMapSwitch);
		size_t end = start;
		if (start < line.size() && line[start] == L'"')
		{
			++start;
			end = line.find(L'"', start);
		}
		else
		{
			end = line.find(L' ', start);
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
	void ApplyRules()
	{
		g_map_name = CurrentMapName();
		g_map_known = !g_map_name.empty();
		const std::string forced = MapSwitchValue();
		if (!forced.empty())
		{
			g_rules = FindMapRules(g_settings, forced);
			g_section_source = "-SpawnControlMap=" + forced;
		}
		else
		{
			g_rules = FindMapRules(g_settings, g_map_name);
			g_section_source = "map " + g_map_name;
		}

		for (ClassEntry& entry : g_classes)
			entry.rule = ResolveClass(g_settings, g_rules, entry.info);

		g_group_counts.assign(g_rules != nullptr ? g_rules->groups.size() : 0, 0);
		g_counts_ready = false; // limits wait for the next census to have real counts

		// A queued removal was planned under the old rules.
		g_queue.clear();
		g_queued.clear();
	}

	Settings LoadSettings()
	{
		const std::string path = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME "/config.json";

		std::ifstream file(path);
		if (!file.is_open())
			throw std::runtime_error("cannot open " + path);

		nlohmann::json root;
		try
		{
			// Last argument: allow // and /* */ comments in config.json.
			root = nlohmann::json::parse(file, nullptr, true, true);
		}
		catch (const nlohmann::json::exception& error)
		{
			throw std::runtime_error(std::string("config.json is not valid JSON: ") + error.what());
		}
		return ParseSettings(root);
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
			Log::GetLog()->info("Config loaded: {} map section(s); using {} for {}",
				g_settings.maps.size(), g_rules != nullptr ? g_rules->name : std::string("no section"), g_section_source);
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
	bool Enqueue(APrimalDinoCharacter* dino, int class_index)
	{
		QueueItem item;
		item.creature = GetWeakReference(dino);
		item.class_index = class_index;
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
		if (!g_refused_since_log.empty())
		{
			if (g_settings.log_removals)
				Log::GetLog()->info("Spawn entries refused in the last {} s: {}", kLogIntervalSeconds, RefusedSummary());
			g_refused_since_log.clear();
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
			if (dino == nullptr || !valid_class || Classify(dino) != Kind::Wild ||
				g_classes[item.class_index].rule.is_protected || !HasAnyRule(g_classes[item.class_index].rule))
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
		g_player_positions = PlayerPositions();

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

		if (!HasAnyRule(rule) || rule.is_protected)
			return;

		CensusItem item;
		item.creature = GetWeakReference(dino);
		item.class_index = class_index;
		item.player_distance = NearestPlayerDistance(dino->GetLocation(), g_player_positions);
		g_census_items.push_back(item);
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
			creatures.push_back({ i, g_census_items[i].class_index, g_census_items[i].player_distance });

		static const std::vector<GroupRule> no_groups;
		const std::vector<GroupRule>& groups = g_rules != nullptr ? g_rules->groups : no_groups;
		const Plan plan = PlanRemovals(rules, groups, creatures);

		// Live counts start from what each rule leaves after the planned removals.
		g_class_counts.assign(g_classes.size(), 0);
		for (size_t c = 0; c < plan.class_counts.size() && c < g_class_counts.size(); ++c)
			g_class_counts[c] = plan.class_counts[c];
		g_group_counts = plan.group_counts;
		g_counts_ready = true;

		// Anything already queued is still on its way out; the plan counted it as present.
		size_t newly_queued = 0;
		for (size_t id : plan.remove)
		{
			APrimalDinoCharacter* dino = g_census_items[id].creature.Get();
			if (dino != nullptr && Enqueue(dino, g_census_items[id].class_index))
				++newly_queued;
		}

		g_building.finished = Clock::now();
		g_building.seconds = std::chrono::duration<double>(g_building.finished - g_census_started).count();
		g_building.planned = plan.remove.size();
		g_building.blocked = plan.blocked;
		g_building.over_limit = plan.over_limit;
		g_building.over_group = plan.over_group;
		g_building.wild_by_class.resize(g_classes.size(), 0);
		g_building.mission_by_class.resize(g_classes.size(), 0);
		g_last = g_building;
		g_have_report = true;
		g_census_items.clear();

		if (plan.remove.size() > 0)
			Log::GetLog()->info("Census: {} wild, {} mission; {} over the rules ({} blocked, {} over limit, {} over group), "
				"{} newly queued, queue {}", g_last.wild, g_last.mission, plan.remove.size(), plan.blocked, plan.over_limit,
				plan.over_group, newly_queued, g_queue.size());
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

	void OnCreatureAppeared(APrimalDinoCharacter* dino)
	{
		if (!g_settings_loaded || g_rules == nullptr || !g_counts_ready)
			return;
		if (Classify(dino) != Kind::Wild)
			return;

		const int class_index = ClassIndexFor(dino->ClassField());
		const ClassRule& rule = g_classes[class_index].rule;
		if (rule.is_protected || !HasAnyRule(rule))
			return;

		// Live counts always mean "after everything already queued is gone".
		AdjustCounts(class_index, +1);
		if (OverOnSpawn(rule, g_class_counts[class_index], g_group_counts, g_rules->groups))
		{
			if (Enqueue(dino, class_index))
				AdjustCounts(class_index, -1);
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
				return false;
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

	DECLARE_HOOK(APrimalDinoCharacter_BeginPlay, void, APrimalDinoCharacter*);

	void Hook_APrimalDinoCharacter_BeginPlay(APrimalDinoCharacter* _this)
	{
		APrimalDinoCharacter_BeginPlay_original(_this);

		const auto start = Clock::now();
		try
		{
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
			LogRemovals();
		}

		if (!g_queue.empty())
		{
			const size_t removed_before = g_removed_total;
			const auto start = Clock::now();
			DrainQueue();
			if (g_removed_total != removed_before)
				g_drain_timing.Add(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
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
		out << g_last.seconds << " s, " << g_last.actors << " actors): " << g_last.wild << " wild, "
			<< g_last.mission << " mission-owned\n";
		out << "Queue " << g_queue.size() << ", removed " << g_removed_total << " (skipped " << g_removed_skipped
			<< "), spawn check avg " << (g_hook_calls > 0 ? g_hook_total_us / g_hook_calls : 0.0) << " us, max "
			<< g_hook_max_us << " us\n";
		out << "Per tick: census slice avg " << g_census_timing.Average() << " ms, max " << g_census_timing.max_ms
			<< " ms; removals avg " << g_drain_timing.Average() << " ms, max " << g_drain_timing.max_ms << " ms ("
			<< g_drain_timing.ticks << " ticks)\n";
		out << "Zone entries: " << g_entry_checks << " checked, " << g_entry_refused << " refused, avg "
			<< (g_entry_checks > 0 ? g_entry_total_us / g_entry_checks : 0.0) << " us, max " << g_entry_max_us << " us"
			<< (g_entry_hook_installed ? "" : " (hook not installed)")
			<< (g_settings.refuse_full_entries ? "" : " (RefuseFullEntries off)") << "\n";

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
				if (entry.rule.is_protected)
					out << " [protected]";
				if (entry.rule.blocked)
					out << " [blocked]";
				if (entry.rule.limit != kNoLimit)
					out << " [limit " << entry.rule.limit << "]";
				out << "\n    " << entry.info.path << "\n";
			}
			return out.str();
		}

		for (size_t c = 0; c < g_classes.size() && c < g_last.wild_by_class.size(); ++c)
		{
			const ClassRule& rule = g_classes[c].rule;
			if (rule.blocked && g_last.wild_by_class[c] > 0)
				out << "  " << g_classes[c].info.short_name << ": " << g_last.wild_by_class[c] << " [blocked]\n";
			else if (rule.limit != kNoLimit)
				out << "  " << g_classes[c].info.short_name << ": " << g_last.wild_by_class[c] << " / " << rule.limit << "\n";
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
						total += g_last.wild_by_class[c];
				}
				out << "  group " << g_rules->groups[g].name << ": " << total << " / " << g_rules->groups[g].max << "\n";
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

	AsaApi::GetCommands().AddConsoleCommand(kCmdReload, &ConsoleReload);
	AsaApi::GetCommands().AddConsoleCommand(kCmdList, &ConsoleList);
	AsaApi::GetCommands().AddRconCommand(kCmdReload, &RconReload);
	AsaApi::GetCommands().AddRconCommand(kCmdList, &RconList);
	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &SafeTimer);
}

// Called by AsaApi when the plugin is unloaded.
extern "C" __declspec(dllexport) void Plugin_Unload()
{
	AsaApi::GetCommands().RemoveOnTimerCallback(kTimerId);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdReload);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdList);
	AsaApi::GetCommands().RemoveRconCommand(kCmdReload);
	AsaApi::GetCommands().RemoveRconCommand(kCmdList);

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
}
