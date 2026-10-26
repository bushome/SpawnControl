// Stand-alone tests for src/SpawnLogic.h. No game, server API DLL or Windows needed.
//
// Build and run from the project folder (any C++20 compiler):
//   cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp && logic_tests.exe configs\config.json
//   clang++ -std=c++20 -I extern/AsaApi/AsaApi/Core/Public -I src tests/logic_tests.cpp -o logic_tests && ./logic_tests configs/config.json
// (ASE folder: use extern\AseApi\version\Core\Public for the json.hpp include path.)
//
// The optional argument is a config.json to validate. Add --shipped after it to also check the file
// against the values of the shipped config:
//   logic_tests.exe configs\config.json --shipped
// configs\config.commented.json holds the same values, so it must pass the same check.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

#include "SpawnLogic.h"

using namespace SpawnControl;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": " #condition "\n"; \
		} \
	} while (false)

#define CHECK_THROWS(expression) \
	do { \
		++g_checks; \
		bool threw = false; \
		try { (void)(expression); } catch (const std::exception&) { threw = true; } \
		if (!threw) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": expected an error from " #expression "\n"; \
		} \
	} while (false)

static Settings Parse(const char* text)
{
	return ParseSettings(nlohmann::json::parse(text, nullptr, true, true));
}

static ClassInfo Class(const std::string& path, std::vector<std::string> parents = {})
{
	ClassInfo info;
	info.path = path;
	info.short_name = ShortNameFromPath(path);
	info.chain.push_back(info.short_name);
	for (std::string& parent : parents)
		info.chain.push_back(std::move(parent));
	return info;
}

static const ClassInfo kOtter = Class("/Game/PrimalEarth/Dinos/Otter/Otter_Character_BP.Otter_Character_BP_C", { "PrimalDinoCharacter" });
static const ClassInfo kSOtter = Class("/SDinoVariants/S-Otter/S-Otter_Character_BP.S-Otter_Character_BP_C", { "PrimalDinoCharacter" });
static const ClassInfo kModOtter = Class("/SomeMod/Otter_Character_BP.Otter_Character_BP_C", { "PrimalDinoCharacter" });
static const ClassInfo kOutpostZombie = Class(
	"/Game/LostColony/Missions/Overworld/Outpost/Defend/Zombies/Zombie_Character_BP_Skinny_Outpost.Zombie_Character_BP_Skinny_Outpost_C",
	{ "Zombie_Character_BP_Skinny_C", "Zombie_Character_BP_C", "PrimalBotCharacter" });
static const ClassInfo kThrall = Class(
	"/Game/LostColony/Dinos/Thrall/Characters/Soldier/Fighter/Thrall_Character_BP_Fighter.Thrall_Character_BP_Fighter_C",
	{ "Thrall_Character_BP_Base_C", "Bot_Character_BP_C", "PrimalBotCharacter" });

static void TestStrings()
{
	CHECK(ClassPathFromReference("Blueprint'/Game/A/Otter_BP.Otter_BP'") == "/Game/A/Otter_BP.Otter_BP_C");
	CHECK(ClassPathFromReference("/Game/A/Otter_BP.Otter_BP_C") == "/Game/A/Otter_BP.Otter_BP_C");
	CHECK(ClassPathFromReference("Class'/Script/ShooterGame'") == "/Script/ShooterGame");
	CHECK(ShortNameFromPath("/Game/A/Otter_BP.Otter_BP_C") == "Otter_BP_C");
	CHECK(ShortNameFromPath("Otter_BP_C") == "Otter_BP_C");
	CHECK(IsPathKey("/Game/A/B.B_C"));
	CHECK(!IsPathKey("B_C"));
	CHECK(KeyMatches("otter_character_bp_c", kOtter));
	CHECK(KeyMatches("Otter_Character_BP_C", kModOtter));
	CHECK(KeyMatches("/game/primalearth/dinos/otter/otter_character_bp.otter_character_bp_c", kOtter));
	CHECK(!KeyMatches("/Game/PrimalEarth/Dinos/Otter/Otter_Character_BP.Otter_Character_BP_C", kModOtter));
	CHECK(!KeyMatches("Otter_Character_BP_C", kSOtter));
}

static void TestParsing()
{
	CHECK_THROWS(Parse("[]"));

	const Settings defaults = Parse("{}");
	CHECK(defaults.warnings.empty());
	CHECK(defaults.removals_per_tick == 10);
	CHECK(defaults.refuse_full_entries);
	CHECK(defaults.protected_paths.size() == 1 && defaults.protected_paths[0] == "/Missions/");
	CHECK(defaults.maps.empty());

	const Settings bad = Parse(R"({
		"General": { "RemovalsPerTick": 0, "CensusIntervalSeconds": "x", "LogRemovals": 1 },
		"Maps": {
			"A": { "Limit": { "X_C": -1, "Y_C": 0, "Z_C": 5 }, "Block": "X_C",
			       "GroupLimit": { "G": { "Max": 0, "Classes": ["X_C", "Y_C"] }, "H": { "Max": 3, "Classes": ["X_C"] } } },
			"B": 7
		}
	})");
	CHECK(bad.removals_per_tick == 10);
	CHECK(bad.census_interval_seconds == 60);
	CHECK(bad.log_removals);
	CHECK(bad.maps.size() == 2);
	CHECK(bad.maps[0].limits.size() == 1 && bad.maps[0].limits[0].first == "Z_C"); // -1 skipped, 0 = none
	CHECK(bad.maps[0].block.empty());
	CHECK(bad.maps[0].groups.size() == 1 && bad.maps[0].groups[0].name == "H"); // one class: kept, warned
	CHECK(bad.warnings.size() >= 7);

	const Settings comments = Parse("// leading comment\n{ /* inline */ \"General\": { \"RemovalsPerTick\": 3 } }");
	CHECK(comments.removals_per_tick == 3);
}

static void TestMapLookup()
{
	// Sections come back in key order (nlohmann sorts objects), so compare by name.
	const Settings settings = Parse(R"({ "Maps": { "LostColony": {}, "TheIsland": {}, "Ragnarok_WP": {} } })");
	auto name = [&](std::string_view map) -> std::string
		{
			const MapRules* rules = FindMapRules(settings, map);
			return rules != nullptr ? rules->name : "";
		};
	CHECK(name("LostColony_WP") == "LostColony");
	CHECK(name("lostcolony") == "LostColony");
	CHECK(name("TheIsland") == "TheIsland");
	CHECK(name("Ragnarok_WP") == "Ragnarok_WP");
	CHECK(name("Ragnarok").empty());
	CHECK(name("Aberration_WP").empty());
}

static void TestResolve()
{
	const Settings settings = Parse(R"({
		"Protect": { "Families": [] },
		"ReportFamilies": { "Thralls": "Thrall_Character_BP_Base_C" },
		"Maps": { "M": {
			"Block": [ "Zombie_Character_BP_Skinny_Outpost_C" ],
			"Limit": { "Otter_Character_BP_C": 250, "/SomeMod/Otter_Character_BP.Otter_Character_BP_C": 10, "Thrall_Character_BP_Fighter_C": 5 },
			"GroupLimit": { "Otters": { "Max": 200, "Classes": [ "Otter_Character_BP_C", "S-Otter_Character_BP_C" ] } }
		} }
	})");
	const MapRules* rules = FindMapRules(settings, "M");
	CHECK(rules != nullptr);

	const ClassRule otter = ResolveClass(settings, rules, kOtter);
	CHECK(!otter.is_protected && !otter.blocked && otter.limit == 250 && otter.groups.size() == 1);

	const ClassRule mod_otter = ResolveClass(settings, rules, kModOtter);
	CHECK(mod_otter.limit == 10);               // full path beats short name
	CHECK(mod_otter.groups.size() == 1);        // still in the group through its short name

	const ClassRule s_otter = ResolveClass(settings, rules, kSOtter);
	CHECK(s_otter.limit == kNoLimit && s_otter.groups.size() == 1);

	const ClassRule outpost = ResolveClass(settings, rules, kOutpostZombie);
	CHECK(outpost.is_protected);                // /Missions/ path wins over the block entry
	CHECK(!outpost.blocked && !HasAnyRule(outpost));

	const ClassRule thrall = ResolveClass(settings, rules, kThrall);
	CHECK(!thrall.is_protected && thrall.limit == 5 && thrall.report_family == 0);

	Settings protect_family = settings;
	protect_family.protected_families = { "Thrall_Character_BP_Base_C" };
	CHECK(ResolveClass(protect_family, rules, kThrall).is_protected);

	const ClassRule no_section = ResolveClass(settings, nullptr, kOtter);
	CHECK(!HasAnyRule(no_section));
}

static void TestPlan()
{
	// Classes: 0 otter (limit 3, group), 1 s-otter (group), 2 blocked, 3 protected, 4 free
	std::vector<ClassRule> rules(5);
	rules[0].limit = 3;
	rules[0].groups = { 0 };
	rules[1].groups = { 0 };
	rules[2].blocked = true;
	rules[3].is_protected = true;
	rules[3].blocked = true;
	std::vector<GroupRule> groups(1);
	groups[0].name = "Otters";
	groups[0].max = 4;

	std::vector<Creature> creatures;
	auto add = [&](int cls, double distance) { creatures.push_back({ creatures.size(), cls, distance }); };
	for (int i = 0; i < 6; ++i) add(0, 100.0 * i);   // ids 0-5, distance 0..500
	for (int i = 0; i < 4; ++i) add(1, 50.0 * i);    // ids 6-9
	add(2, 10); add(2, 20);                          // ids 10-11
	add(3, 999);                                     // id 12, protected
	add(4, 5);                                       // id 13

	const Plan plan = PlanRemovals(rules, groups, creatures);
	CHECK(plan.blocked == 2);
	CHECK(plan.over_limit == 3);                     // otters 6 -> 3
	CHECK(plan.class_counts[0] <= 3);
	CHECK(plan.group_counts[0] == 4);
	CHECK(plan.over_group == 3);                     // 3 + 4 = 7 -> 4
	CHECK(plan.class_counts[3] == 0);                // protected is never counted
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), 12) == plan.remove.end());
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), 13) == plan.remove.end());

	// Blocked first, then the farthest otters (ids 5, 4, 3).
	CHECK(plan.remove.size() == 8);
	CHECK(plan.remove[0] == 11 && plan.remove[1] == 10);
	CHECK(plan.remove[2] == 5 && plan.remove[3] == 4 && plan.remove[4] == 3);

	// Group trimming takes from the larger member: s-otters (4 left) before otters (3 left).
	CHECK(plan.class_counts[0] + plan.class_counts[1] == 4);
	CHECK(plan.class_counts[0] >= 1 && plan.class_counts[1] >= 1);

	// Nothing over: nothing removed.
	std::vector<Creature> few = { { 0, 0, 1 }, { 1, 1, 1 } };
	CHECK(PlanRemovals(rules, groups, few).remove.empty());

	// Out-of-range class indices are ignored, not crashed on.
	std::vector<Creature> odd = { { 0, 99, 1 }, { 1, -1, 1 } };
	CHECK(PlanRemovals(rules, groups, odd).remove.empty());
}

static void TestOverOnSpawn()
{
	std::vector<GroupRule> groups(1);
	groups[0].max = 2;
	ClassRule rule;
	rule.limit = 3;
	rule.groups = { 0 };
	CHECK(!OverOnSpawn(rule, 3, { 2 }, groups));
	CHECK(OverOnSpawn(rule, 4, { 2 }, groups));
	CHECK(OverOnSpawn(rule, 1, { 3 }, groups));
	ClassRule blocked;
	blocked.blocked = true;
	CHECK(OverOnSpawn(blocked, 1, {}, groups));
	blocked.is_protected = true;
	CHECK(!OverOnSpawn(blocked, 1, {}, groups));
}

static void TestIsFull()
{
	std::vector<GroupRule> groups(1);
	groups[0].max = 2;
	ClassRule rule;
	rule.limit = 3;
	rule.groups = { 0 };
	CHECK(!IsFull(rule, 2, { 1 }, groups));
	CHECK(IsFull(rule, 3, { 1 }, groups));      // at the limit: no more
	CHECK(IsFull(rule, 0, { 2 }, groups));      // group at its max
	ClassRule free_rule;
	CHECK(!IsFull(free_rule, 10000, {}, groups));
	ClassRule blocked;
	blocked.blocked = true;
	CHECK(IsFull(blocked, 0, {}, groups));
	blocked.is_protected = true;
	CHECK(!IsFull(blocked, 0, {}, groups));
}

static void TestRefresh()
{
	// Quota: 24,000 creatures over 24 h = 1,000 per hour = 16.67 per minute; the fraction carries over.
	double carry = 0;
	size_t total = 0;
	for (int minute = 0; minute < 60; ++minute)
		total += RefreshQuota(24000, 60, 24, carry);
	CHECK(total == 1000 || total == 999);
	carry = 0;
	CHECK(RefreshQuota(24000, 60, 0, carry) == 0);
	CHECK(RefreshQuota(0, 60, 24, carry) == 0);
	CHECK(RefreshQuota(24000, 0, 24, carry) == 0);

	// Oldest first, then farthest from players; too young or too close never picked.
	const std::vector<RefreshCandidate> candidates = {
		{ 0, 7200, 50000 }, { 1, 9000, 40000 }, { 2, 9000, 60000 },
		{ 3, 600, 90000 },   // too young
		{ 4, 99999, 1000 },  // too close to a player
	};
	const std::vector<size_t> picked = PlanRefresh(candidates, 2, 3600, 30000);
	CHECK(picked.size() == 2 && picked[0] == 2 && picked[1] == 1);
	const std::vector<size_t> all = PlanRefresh(candidates, 10, 3600, 30000);
	CHECK(all.size() == 3);
	CHECK(PlanRefresh(candidates, 0, 3600, 30000).empty());

	Settings settings;
	ClassInfo thrall{ "Thrall_Character_BP_Soldier_C", "/Game/LostColony/Thrall/Thrall_Character_BP_Soldier.Thrall_Character_BP_Soldier_C", {} };
	ClassInfo rex{ "Rex_Character_BP_C", "/Game/PrimalEarth/Dinos/Rex/Rex_Character_BP.Rex_Character_BP_C", {} };
	CHECK(SkipsRefresh(settings, thrall));
	CHECK(!SkipsRefresh(settings, rex));
}

static void TestConfigFile(const std::string& path, bool shipped_expectations)
{
	std::ifstream file(path);
	CHECK(file.is_open());
	if (!file.is_open())
		return;

	Settings settings;
	try
	{
		settings = ParseSettings(nlohmann::json::parse(file, nullptr, true, true));
	}
	catch (const std::exception& error)
	{
		++g_checks;
		++g_failures;
		std::cerr << "FAILED: " << path << " did not parse: " << error.what() << "\n";
		return;
	}
	for (const std::string& warning : settings.warnings)
		std::cerr << "warning: " << warning << "\n";
	CHECK(settings.warnings.empty());

	if (!shipped_expectations)
		return;

	CHECK(settings.removals_per_tick == 10);
	CHECK(settings.census_actors_per_tick == 5000);
	CHECK(settings.census_interval_seconds == 60);
	CHECK(settings.near_player_distance == 10000.0);
	CHECK(settings.log_removals);
	CHECK(settings.refuse_full_entries);
	CHECK(settings.protected_paths.size() == 1 && settings.protected_paths[0] == "/Missions/");
	CHECK(settings.protected_families.empty());
	CHECK(settings.report_families.size() == 1 && settings.report_families[0].base_class == "Thrall_Character_BP_Base_C");
	CHECK(settings.refresh_hours == 24);
	CHECK(settings.refresh_min_age_minutes == 60);
	CHECK(settings.refresh_player_distance == 30000.0);
	CHECK(settings.refresh_recent_damage_seconds == 300);
	CHECK(settings.refresh_skip_paths.size() == 3);

	const MapRules* lost_colony = FindMapRules(settings, "LostColony_WP");
	CHECK(lost_colony != nullptr);
	if (lost_colony == nullptr)
		return;
	CHECK(lost_colony->block.empty());
	CHECK(lost_colony->limits.size() == 24);
	CHECK(lost_colony->groups.size() == 1 && lost_colony->groups[0].max == 200 && lost_colony->groups[0].classes.size() == 2);
	CHECK(ResolveClass(settings, lost_colony, kOtter).groups.size() == 1);
	CHECK(ResolveClass(settings, lost_colony, kOutpostZombie).is_protected);
}

int main(int argc, char** argv)
{
	TestStrings();
	TestParsing();
	TestMapLookup();
	TestResolve();
	TestPlan();
	TestOverOnSpawn();
	TestIsFull();
	TestRefresh();

	if (argc > 1)
		TestConfigFile(argv[1], argc > 2 && std::string(argv[2]) == "--shipped");

	std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
