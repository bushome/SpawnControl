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
	CHECK(defaults.ignore_summoned);
	CHECK(!Parse(R"({ "General": { "IgnoreSummoned": false } })").ignore_summoned);
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

static void TestDensity()
{
	// Parsing: a class, a named mix, "*", and bad entries.
	const Settings parsed = Parse(R"({ "Maps": { "M": { "Density": {
		"Bary_C": { "Max": 8, "AreaInFoundations": 30 },
		"Clump": { "Max": 5, "AreaInFoundations": 10, "Classes": [ "A_C", "B_C" ] },
		"*": { "Max": 40, "AreaInFoundations": 999 },
		"Bad": { "Max": 0, "AreaInFoundations": 10 },
		"Worse": 5 } } } })");
	const MapRules& map = parsed.maps.front();
	CHECK(map.density.size() == 3);
	CHECK(map.density[0].name == "*" || map.density[0].name == "Bary_C" || map.density[0].name == "Clump");
	for (const DensityRule& rule : map.density)
	{
		if (rule.name == "Bary_C")
			CHECK(rule.max == 8 && rule.size == 30 * kFoundation && rule.classes.size() == 1 && !rule.all);
		if (rule.name == "Clump")
			CHECK(rule.classes.size() == 2 && rule.size == 10 * kFoundation);
		if (rule.name == "*")
			CHECK(rule.all && rule.size == kMaxDensityArea * kFoundation);
	}
	CHECK(parsed.warnings.size() == 4); // area capped, Bad: Max and skipped, Worse skipped

	// Resolve: membership by key, Classes and "*"; density alone counts as a rule.
	MapRules rules_map;
	rules_map.density.resize(2);
	rules_map.density[0].classes = { "Bary_C" };
	rules_map.density[1].all = true;
	Settings settings;
	ClassInfo bary;
	bary.short_name = "Bary_C";
	bary.path = "/Game/Bary.Bary_C";
	bary.chain = { "Bary_C" };
	const ClassRule bary_rule = ResolveClass(settings, &rules_map, bary);
	CHECK(bary_rule.density.size() == 2 && HasAnyRule(bary_rule));
	ClassInfo mission;
	mission.short_name = "M_C";
	mission.path = "/Game/Missions/M.M_C";
	CHECK(ResolveClass(settings, &rules_map, mission).density.empty()); // protected

	// Planning: a clump of 10 at one spot, 3 strays far apart, max 4 per 1000-unit cube.
	std::vector<ClassRule> rules(2);
	rules[0].density = { 0 };
	rules[1].density = { 0 };
	rules[1].limit = 100;
	std::vector<DensityRule> density(1);
	density[0].max = 4;
	density[0].size = 1000;
	std::vector<Creature> creatures;
	auto add = [&](int cls, double x, double y, double distance)
		{
			Creature c;
			c.id = creatures.size();
			c.class_index = cls;
			c.player_distance = distance;
			c.x = x;
			c.y = y;
			creatures.push_back(c);
		};
	for (int i = 0; i < 10; ++i) add(i % 2, 10.0 * i, 0, 100.0 * i); // ids 0-9, one clump
	add(0, 50000, 0, 1); add(0, 0, 50000, 1); add(1, -50000, 0, 1);  // ids 10-12, alone
	const Plan plan = PlanRemovals(rules, {}, creatures, density);
	CHECK(plan.too_dense == 6);                    // the clump ends at 4
	CHECK(plan.hotspots.size() == 1 && !plan.hotspots[0].empty() && plan.hotspots[0][0].count == 10);
	CHECK(plan.hotspots[0][0].x == 45 && plan.hotspots[0][0].classes.size() == 2 && plan.hotspots[0][0].classes[0].first == 5);
	CHECK(plan.hotspots[0].size() == 1);           // the shifted grid's view of the same clump is not listed twice
	CHECK(plan.class_counts[0] + plan.class_counts[1] == 7);
	for (size_t id : plan.remove)
		CHECK(id < 10);                            // strays never touched
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), size_t(9)) != plan.remove.end()); // farthest from players first
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), size_t(0)) == plan.remove.end()); // nearest stays

	// A clump straddling a cube edge (x = 990 and 1010) is still capped by the shifted grid.
	std::vector<Creature> edge;
	for (int i = 0; i < 10; ++i)
	{
		Creature c; c.id = edge.size(); c.class_index = 0; c.x = (i % 2 == 0) ? 990 : 1010; c.y = 100 + i; edge.push_back(c);
	}
	CHECK(PlanRemovals(rules, {}, edge, density).too_dense == 6);

	// Two clumps a cube and a half apart are separate; each keeps its max.
	std::vector<Creature> two;
	for (int i = 0; i < 6; ++i)
	{
		Creature c; c.id = two.size(); c.class_index = 0; c.x = 0; c.y = i; two.push_back(c);
	}
	for (int i = 0; i < 6; ++i)
	{
		Creature c; c.id = two.size(); c.class_index = 0; c.x = 2500; c.y = i; two.push_back(c);
	}
	const Plan split = PlanRemovals(rules, {}, two, density);
	CHECK(split.too_dense == 4);
	CHECK(split.hotspots[0].size() == 2 && split.hotspots[0][0].count == 6 && split.hotspots[0][1].count == 6);
	CHECK(PlanRemovals(rules, {}, two).remove.empty()); // no density rules, nothing to do

	// Caves and skip areas are left alone; Exclude keeps a class out of a rule.
	std::vector<Creature> cave = creatures;
	for (int i = 0; i < 10; ++i) cave[i].in_cave = true;
	CHECK(PlanRemovals(rules, {}, cave, density).too_dense == 0);
	DensitySkip no_caves;
	no_caves.caves = false;
	CHECK(PlanRemovals(rules, {}, cave, density, no_caves).too_dense == 6);
	DensitySkip city;
	city.areas.push_back({ "City", 40, 0, 200 });
	CHECK(PlanRemovals(rules, {}, creatures, density, city).too_dense == 0);
	city.areas[0].x = 5000;
	CHECK(PlanRemovals(rules, {}, creatures, density, city).too_dense == 6);

	const Settings skip_parsed = Parse(R"({ "Maps": { "M": {
		"Density": { "*": { "Max": 40, "AreaInFoundations": 30, "Exclude": [ "Nameless_C" ] } },
		"DensitySkip": { "Caves": false, "Areas": [ { "Name": "City", "Location": [ 1000, -2000, 50 ], "RadiusInFoundations": 100 },
			{ "Location": [ 1 ] } ] } } } })");
	const MapRules& skip_map = skip_parsed.maps.front();
	CHECK(!skip_map.density_skip.caves);
	CHECK(skip_map.density_skip.areas.size() == 1 && skip_map.density_skip.areas[0].name == "City" &&
		skip_map.density_skip.areas[0].y == -2000 && skip_map.density_skip.areas[0].radius == 100 * kFoundation);
	CHECK(skip_parsed.warnings.size() == 1); // the bad area is skipped
	ClassInfo nameless;
	nameless.short_name = "Nameless_C";
	nameless.path = "/Game/Aberration/Nameless.Nameless_C";
	CHECK(ResolveClass(settings, &skip_map, nameless).density.empty());
	CHECK(ResolveClass(settings, &skip_map, bary).density.size() == 1);

	// Exclude by parent class: one entry keeps every thrall out.
	MapRules thrall_map;
	thrall_map.density.resize(1);
	thrall_map.density[0].all = true;
	thrall_map.density[0].exclude = { "Thrall_Character_BP_Base_C" };
	ClassInfo thrall;
	thrall.short_name = "Thrall_Archer_BP_C";
	thrall.path = "/Game/LostColony/Thrall/Thrall_Archer_BP.Thrall_Archer_BP_C";
	thrall.chain = { "Thrall_Archer_BP_C", "Thrall_Character_BP_Base_C", "PrimalDinoCharacter" };
	CHECK(ResolveClass(settings, &thrall_map, thrall).density.empty());
	CHECK(ResolveClass(settings, &thrall_map, bary).density.size() == 1);

	// Exclude by part of a path: one entry for a whole folder.
	thrall_map.density[0].exclude = { "/ZombieDino/" };
	ClassInfo corrupted;
	corrupted.short_name = "Stego_Character_BP_Corrupted_Zombie_C";
	corrupted.path = "/Game/LostColony/Dinos/ZombieDino/Stego/Stego_Character_BP_Corrupted_Zombie.Stego_Character_BP_Corrupted_Zombie_C";
	corrupted.chain = { corrupted.short_name, "Stego_Character_BP_C", "Dino_Character_BP_C", "PrimalDinoCharacter" };
	CHECK(ResolveClass(settings, &thrall_map, corrupted).density.empty());
	CHECK(ResolveClass(settings, &thrall_map, bary).density.size() == 1);
}

static void TestBlockAreas()
{
	const Settings parsed = Parse(R"({ "Maps": { "M": { "BlockAreas": [
		{ "Name": "Spawner", "Location": [ 1000, 2000, -60000 ], "RadiusInFoundations": 10 },
		{ "Location": [ 0, 0 ], "RadiusInFoundations": 5 },
		{ "Location": [ 0, 0, 0, 0 ], "RadiusInFoundations": 5 },
		{ "Name": "NoRadius", "Location": [ 0, 0 ] } ] } } })");
	const MapRules& map = parsed.maps.front();
	CHECK(map.block_areas.size() == 2);
	CHECK(map.block_areas[0].has_z && map.block_areas[0].z == -60000 && map.block_areas[0].radius == 10 * kFoundation);
	CHECK(!map.block_areas[1].has_z);
	CHECK(parsed.warnings.size() == 2);

	// A sphere leaves the surface above it alone; a column does not care about height.
	CHECK(BlockAreaAt(map.block_areas, 1000, 2000, -60000) == 0);
	CHECK(BlockAreaAt(map.block_areas, 1000, 2000, 10000) == -1);
	CHECK(BlockAreaAt(map.block_areas, 100, 100, 99999) == 1);
	CHECK(BlockAreaAt(map.block_areas, 5000, 5000, 0) == -1);

	CHECK(!ZoneShouldStop(2, 0));
	CHECK(ZoneShouldStop(3, 0));
	CHECK(!ZoneShouldStop(10, 1));

	// Planning: creatures in a block area go first, whatever their class, and are not counted.
	std::vector<ClassRule> rules(2);
	rules[0].limit = 1;
	rules[1].is_protected = true;
	std::vector<Creature> creatures;
	for (int i = 0; i < 4; ++i)
	{
		Creature c; c.id = creatures.size(); c.class_index = 0; c.player_distance = i; c.in_block_area = i < 2; creatures.push_back(c);
	}
	Creature unruled; unruled.id = creatures.size(); unruled.class_index = 1; unruled.in_block_area = true; creatures.push_back(unruled);
	const Plan plan = PlanRemovals(rules, {}, creatures);
	CHECK(plan.in_block_area == 2);              // the protected one stays
	CHECK(plan.remove.size() == 3);              // 2 in the area, then 1 over the limit of 1
	CHECK(plan.class_counts[0] == 1);
	CHECK(plan.remove[0] == 0 && plan.remove[1] == 1);

	// DensitySkip areas still parse through the same reader.
	const Settings skip = Parse(R"({ "Maps": { "M": { "DensitySkip": { "Areas": [ { "Location": [ 5, 6, 7 ], "RadiusInFoundations": 2 } ] } } } })");
	CHECK(skip.maps.front().density_skip.areas.size() == 1 && skip.maps.front().density_skip.areas[0].y == 6);
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
	CHECK(settings.ignore_summoned);
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
	TestDensity();
	TestBlockAreas();
	TestOverOnSpawn();
	TestIsFull();
	TestRefresh();

	if (argc > 1)
		TestConfigFile(argv[1], argc > 2 && std::string(argv[2]) == "--shipped");

	std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
