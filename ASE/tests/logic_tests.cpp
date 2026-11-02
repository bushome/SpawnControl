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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

static void TestOverlap()
{
	const Settings parsed = Parse(R"({ "Maps": { "M": { "Overlap": { "Fraction": 0.6, "Exclude": [ "Thrall_Character_BP_Base_C" ] } },
		"N": { "Overlap": { "Fraction": 5 } }, "O": {} } })");
	CHECK(parsed.maps.size() == 3);
	CHECK(parsed.maps[0].overlap.enabled && parsed.maps[0].overlap.fraction == 0.6 && parsed.maps[0].overlap.exclude.size() == 1);
	CHECK(parsed.maps[1].overlap.enabled && parsed.maps[1].overlap.fraction == 0.5);
	CHECK(!parsed.maps[2].overlap.enabled);
	CHECK(parsed.warnings.size() == 1);

	Settings settings;
	ClassInfo raptor;
	raptor.short_name = "Raptor_C";
	raptor.path = "/Game/Raptor.Raptor_C";
	raptor.chain = { "Raptor_C", "PrimalDinoCharacter" };
	ClassInfo thrall;
	thrall.short_name = "Thrall_Archer_C";
	thrall.path = "/Game/Thrall_Archer.Thrall_Archer_C";
	thrall.chain = { "Thrall_Archer_C", "Thrall_Character_BP_Base_C", "PrimalDinoCharacter" };
	const ClassRule raptor_rule = ResolveClass(settings, &parsed.maps[0], raptor);
	CHECK(raptor_rule.overlap && HasAnyRule(raptor_rule));
	CHECK(!ResolveClass(settings, &parsed.maps[0], thrall).overlap);
	CHECK(!ResolveClass(settings, &parsed.maps[2], raptor).overlap);

	// Three raptors stacked on one spot, one a little apart, one far away; radius 100, fraction 0.5:
	// overlapping means centres closer than 100.
	std::vector<ClassRule> rules(2);
	rules[0].overlap = true;
	std::vector<Creature> creatures;
	auto add = [&](int cls, double x, double distance, bool attached = false)
		{
			Creature c;
			c.id = creatures.size();
			c.class_index = cls;
			c.x = x;
			c.radius = 100;
			c.player_distance = distance;
			c.attached = attached;
			creatures.push_back(c);
		};
	add(0, 0, 500); add(0, 10, 100); add(0, 20, 900);   // ids 0-2: one stack
	add(0, 150, 300);                                   // id 3: touching, not inside
	add(0, 9000, 50);                                   // id 4: alone
	add(0, 5, 10, true);                                // id 5: a rider, ignored
	add(1, 0, 1);                                       // id 6: class without the rule
	const Plan plan = PlanRemovals(rules, {}, creatures);
	CHECK(plan.overlapping == 2);
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), size_t(1)) == plan.remove.end()); // nearest a player stays
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), size_t(0)) != plan.remove.end());
	CHECK(std::find(plan.remove.begin(), plan.remove.end(), size_t(2)) != plan.remove.end());
	CHECK(plan.overlap_samples.size() == 2 && plan.overlap_samples[0].removed_class == 0);
	CHECK(plan.class_counts[0] == 4);                  // 6 creatures of class 0 less the 2 removed

	// A large creature reaches into a neighbouring cell: radius 1000 next to radius 100, 400 apart.
	std::vector<Creature> mixed;
	Creature big; big.id = 0; big.class_index = 0; big.radius = 1000; big.player_distance = 1; mixed.push_back(big);
	Creature small; small.id = 1; small.class_index = 0; small.radius = 100; small.x = 400; small.player_distance = 2; mixed.push_back(small);
	CHECK(PlanRemovals(rules, {}, mixed).overlapping == 1);
	CHECK(PlanRemovals(rules, {}, mixed, {}, {}, 0.3).overlapping == 0); // 400 > 1100 * 0.3

	// A bad radius (a creature being torn down) is ignored rather than making every pair a neighbour.
	std::vector<Creature> bad = creatures;
	bad[4].radius = 1.0e30;
	CHECK(PlanRemovals(rules, {}, bad).overlapping == 2);
	bad[4].radius = std::numeric_limits<double>::quiet_NaN();
	CHECK(PlanRemovals(rules, {}, bad).overlapping == 2);

	// Overlap removals count before limits, so a limit does not also take a creature that was not stacked.
	rules[0].limit = 4;
	const Plan both = PlanRemovals(rules, {}, creatures);
	CHECK(both.overlapping == 2 && both.over_limit == 0);
}

static void TestConfigChecks()
{
	// Trailing commas: removed and reported by line; strings and comments untouched.
	std::string text = "{\n  \"A\": [ 1, 2, ],\n  \"B\": \"x, ]\", // a, ]\n  \"C\": { \"D\": 1, /* , } */ },\n}";
	const std::vector<int> lines = StripTrailingCommas(text);
	CHECK(lines.size() == 3 && lines[0] == 2 && lines[1] == 4 && lines[2] == 4);
	const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, true, true);
	CHECK(parsed["A"].size() == 2 && parsed["B"] == "x, ]" && parsed["C"]["D"] == 1);
	std::string escaped = "{ \"A\": \"quote \\\" , ]\", }";
	CHECK(StripTrailingCommas(escaped).size() == 1 && nlohmann::json::parse(escaped)["A"] == "quote \" , ]");
	std::string clean = "{ \"A\": [ 1, 2 ] }";
	CHECK(StripTrailingCommas(clean).empty() && clean == "{ \"A\": [ 1, 2 ] }");

	const Settings forgiving = ParseConfigText("{ \"General\": { \"RemovalsPerTick\": 5, }, }");
	CHECK(forgiving.removals_per_tick == 5 && forgiving.warnings.size() == 2 && forgiving.warnings[0].rfind("line 1", 0) == 0);
	CHECK_THROWS(ParseConfigText("{ \"A\": 1 \"B\": 2 }")); // a missing comma still fails

	// Unknown keys: today's two misplacements, and a typo.
	const Settings misplaced = Parse(R"({ "Maps": {
		"LostColony": { "Limit": {}, "DensitySkip": { "Caves": true, "BlockAreas": [] }, "Overlapp": {} },
		"Overlap": { "Fraction": 0.5 } },
		"General": { "RemovalPerTick": 5 } })");
	auto has = [&](const std::string& part)
		{
			for (const std::string& warning : misplaced.warnings)
			{
				if (warning.find(part) != std::string::npos)
					return true;
			}
			return false;
		};
	CHECK(has("Maps.Overlap is a rule, not a map"));
	CHECK(has("Maps.LostColony.DensitySkip.BlockAreas is not a known setting here; ignored (it belongs directly inside a map section"));
	CHECK(has("Maps.LostColony.Overlapp is not a known setting"));
	CHECK(has("General.RemovalPerTick is not a known setting"));
	CHECK(misplaced.maps.size() == 2);
}

static void TestUnderMesh()
{
	const Settings parsed = Parse(R"({ "Maps": {
		"A": { "UnderMesh": { "ChecksPerSecond": 50, "Exclude": [ "/ZombieDino/" ] } },
		"B": { "UnderMesh": { "ChecksPerSecond": 1900 } },
		"C": { "UnderMesh": {} },
		"D": {} } })");
	CHECK(parsed.maps[0].under_mesh.enabled && parsed.maps[0].under_mesh.checks_per_second == 50 && parsed.maps[0].under_mesh.exclude.size() == 1);
	CHECK(parsed.maps[1].under_mesh.checks_per_second == 1000);
	CHECK(parsed.maps[2].under_mesh.enabled && parsed.maps[2].under_mesh.checks_per_second == 20);
	CHECK(!parsed.maps[3].under_mesh.enabled);
	CHECK(parsed.warnings.size() == 1);

	Settings settings;
	ClassInfo raptor;
	raptor.short_name = "Raptor_C";
	raptor.path = "/Game/Raptor.Raptor_C";
	raptor.chain = { "Raptor_C", "PrimalDinoCharacter" };
	ClassInfo corrupted;
	corrupted.short_name = "Raptor_Corrupted_C";
	corrupted.path = "/Game/ZombieDino/Raptor_Corrupted.Raptor_Corrupted_C";
	CHECK(ResolveClass(settings, &parsed.maps[0], raptor).under_mesh && HasAnyRule(ResolveClass(settings, &parsed.maps[0], raptor)));
	CHECK(!ResolveClass(settings, &parsed.maps[0], corrupted).under_mesh);
	CHECK(!ResolveClass(settings, &parsed.maps[3], raptor).under_mesh);

	CHECK(!parsed.maps[0].under_mesh.remove && !parsed.maps[0].under_mesh.include_stasis);
	CHECK(Parse(R"({ "Maps": { "A": { "UnderMesh": { "IncludeStasis": true } } } })").maps[0].under_mesh.include_stasis);
	const Settings removing = Parse(R"({ "Maps": { "A": { "UnderMesh": { "Remove": true } }, "B": { "UnderMesh": { "Remove": "yes" } } } })");
	CHECK(removing.maps[0].under_mesh.remove && !removing.maps[1].under_mesh.remove && removing.warnings.size() == 1);

	// Confirmation: two "below" results on different censuses; overlapping or clear resets.
	uint32_t first = 0;
	CHECK(!ConfirmUnderMesh(true, false, 5, first) && first == 5);
	CHECK(!ConfirmUnderMesh(true, false, 5, first));            // same census again: not yet
	CHECK(ConfirmUnderMesh(true, false, 6, first));             // a later census: confirmed
	first = 0;
	CHECK(!ConfirmUnderMesh(true, true, 5, first) && first == 0); // overlapping only: never
	CHECK(!ConfirmUnderMesh(true, false, 5, first) && first == 5);
	CHECK(!ConfirmUnderMesh(false, false, 6, first) && first == 0); // clear in between: starts over
	CHECK(!ConfirmUnderMesh(true, false, 7, first) && first == 7);

	const Settings typo = Parse(R"({ "Maps": { "A": { "UnderMesh": { "ChecksPerTick": 5 } } } })");
	CHECK(typo.warnings.size() == 1 && typo.warnings[0].find("UnderMesh.ChecksPerTick") != std::string::npos);

	// RemoveNoGround: off unless set, separate from Remove.
	CHECK(!parsed.maps[0].under_mesh.remove_no_ground);
	const Settings fallen = Parse(R"({ "Maps": { "A": { "UnderMesh": { "RemoveNoGround": true } } } })");
	CHECK(fallen.maps[0].under_mesh.remove_no_ground && !fallen.maps[0].under_mesh.remove && fallen.warnings.empty());

	// TraceGround: off unless set, a known key, and checked for type.
	CHECK(!parsed.maps[0].under_mesh.trace_ground);
	const Settings tracing = Parse(R"({ "Maps": { "A": { "UnderMesh": { "TraceGround": true } }, "B": { "UnderMesh": { "TraceGround": 1 } } } })");
	CHECK(tracing.maps[0].under_mesh.trace_ground && !tracing.maps[1].under_mesh.trace_ground);
	CHECK(tracing.warnings.size() == 1 && tracing.warnings[0].find("TraceGround must be true or false") != std::string::npos);

	// IgnoreFoliage: on unless set false, checked for type; foliage hits recognised by class name.
	CHECK(parsed.maps[0].under_mesh.ignore_foliage);
	const Settings foliage = Parse(R"({ "Maps": { "A": { "UnderMesh": { "IgnoreFoliage": false } }, "B": { "UnderMesh": { "IgnoreFoliage": "no" } } } })");
	CHECK(!foliage.maps[0].under_mesh.ignore_foliage && foliage.maps[1].under_mesh.ignore_foliage);
	CHECK(foliage.warnings.size() == 1 && foliage.warnings[0].find("IgnoreFoliage must be true or false") != std::string::npos);
	CHECK(IsFoliageHit("FoliageInstancedStaticMeshComponent") && IsFoliageHit("InteractiveFoliageComponent"));
	CHECK(!IsFoliageHit("StaticMeshComponent") && !IsFoliageHit("LandscapeHeightfieldCollisionComponent") && !IsFoliageHit("nothing"));

	// Heights on suspect lines, in foundations to one decimal.
	CHECK(MeshHeights(4 * kFoundation, GroundBelow::NotTraced, 0) == "hit 4.0 fdn above");
	CHECK(MeshHeights(1234.0, GroundBelow::Found, 75.0) == "hit 4.1 fdn above, ground 0.3 fdn below");
	CHECK(MeshHeights(-150.0, GroundBelow::None, 0) == "hit 0.5 fdn below, no ground within 100 fdn below");
	CHECK(MeshHeights(0, GroundBelow::Found, 0) == "hit 0.0 fdn above, ground 0.0 fdn below");
}

static void TestRepeatSpots()
{
	std::vector<RemovalPoint> points;
	for (int i = 0; i < 5; ++i) points.push_back({ 1000.0 + i * 100, 2000, -500, 0 });   // a spot of 5, class 0
	points.push_back({ 1200, 2100, -500, 1 });                                            // and one of class 1 in it
	for (int i = 0; i < 3; ++i) points.push_back({ 90000.0 + i * 50, 0, 0, 2 });           // a spot of 3
	points.push_back({ 50000, 50000, 0, 3 }); points.push_back({ 51000, 50000, 0, 3 });    // only 2: not a spot
	const std::vector<RepeatSpot> spots = FindRepeatSpots(points, 3000, 3, {}, 10);
	CHECK(spots.size() == 2);
	CHECK(spots[0].count == 6 && spots[0].classes[0].first == 5 && spots[0].classes[0].second == 0);
	CHECK(std::abs(spots[0].x - 1200) < 1 && spots[0].radius_foundations >= 5);
	CHECK(spots[1].count == 3);

	// A spot already inside a block area is not suggested again.
	std::vector<BlockArea> blocked(1);
	blocked[0].x = 1200; blocked[0].y = 2000; blocked[0].z = -500; blocked[0].has_z = true; blocked[0].radius = 5000;
	const std::vector<RepeatSpot> left = FindRepeatSpots(points, 3000, 3, blocked, 10);
	CHECK(left.size() == 1 && left[0].count == 3);
	CHECK(FindRepeatSpots({}, 3000, 3, {}, 10).empty());

	// Ground readings are summed per spot; untraced points add nothing.
	CHECK(RepeatSpotGround(spots[0]).empty());
	std::vector<RemovalPoint> traced;
	traced.push_back({ 0, 0, 0, 0, GroundBelow::Found, 100 });
	traced.push_back({ 50, 0, 0, 0, GroundBelow::Found, 300 });
	traced.push_back({ 100, 0, 0, 0, GroundBelow::Found, 900 });
	traced.push_back({ 150, 0, 0, 0, GroundBelow::None, 0 });
	traced.push_back({ 200, 0, 0, 0, GroundBelow::NotTraced, 0 });
	const std::vector<RepeatSpot> mixed = FindRepeatSpots(traced, 3000, 3, {}, 10);
	CHECK(mixed.size() == 1 && mixed[0].count == 5 && mixed[0].ground_near == 2 && mixed[0].ground_far == 1 && mixed[0].ground_none == 1);
	CHECK(RepeatSpotGround(mixed[0]) == "ground below: 2 within 1 fdn, 1 farther, 1 none");
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

static bool HasLine(const std::vector<std::string>& lines, const std::string& text)
{
	return std::find(lines.begin(), lines.end(), text) != lines.end();
}

static bool AnyContains(const std::vector<std::string>& lines, const std::string& text)
{
	return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

static int LimitOf(const MapRules& rules, const std::string& key)
{
	for (const auto& [name, max] : rules.limits)
	{
		if (name == key)
			return max;
	}
	return -1;
}

static void TestDefaultSection()
{
	const Settings settings = ParseConfigText(R"({
		"Default": {
			"Limit": { "Dodo_Character_BP_C": 250, "Lystro_Character_BP_C": 250 },
			"Overlap": { "Fraction": 0.5, "Exclude": [ "Thrall" ] },
			"UnderMesh": { "ChecksPerSecond": 20, "Remove": true, "Exclude": [ "/Nameless/" ] }
		},
		"Maps": {
			"Aberration": {
				"Limit": { "Dodo_Character_BP_C": 300, "Lystro_Character_BP_C": null, "Lightbug_Character_BP_C": 400 },
				"UnderMesh": { "ChecksPerSecond": 40 }
			},
			"Island": { "Overlap": { "Exclude": [] } }
		}
	})");
	CHECK(settings.warnings.empty());
	CHECK(settings.has_default);
	const MapRules* aberration = FindMapRules(settings, "Aberration_WP");
	CHECK(aberration != nullptr);
	if (aberration == nullptr)
		return;
	CHECK(LimitOf(*aberration, "Dodo_Character_BP_C") == 300);
	CHECK(LimitOf(*aberration, "Lystro_Character_BP_C") == -1);   // null deletes the default
	CHECK(LimitOf(*aberration, "Lightbug_Character_BP_C") == 400);
	CHECK(aberration->limits.size() == 2);
	CHECK(aberration->under_mesh.enabled && aberration->under_mesh.checks_per_second == 40 && aberration->under_mesh.remove);
	CHECK(aberration->under_mesh.exclude.size() == 1);
	CHECK(aberration->overlap.enabled && aberration->overlap.fraction == 0.5 && aberration->overlap.exclude.size() == 1);

	const MapRules* island = FindMapRules(settings, "TheIsland_WP");
	CHECK(island == nullptr);   // matched by section name only
	island = FindMapRules(settings, "Island");
	CHECK(island != nullptr && island->overlap.exclude.empty() && LimitOf(*island, "Dodo_Character_BP_C") == 250);

	// A map with no section runs the Default rules (the plugin falls back to default_rules).
	CHECK(FindMapRules(settings, "Ragnarok_WP") == nullptr);
	CHECK(settings.default_rules.under_mesh.checks_per_second == 20 && settings.default_rules.limits.size() == 2);

	const std::vector<std::string> lines = DescribeRules(*aberration);
	CHECK(HasLine(lines, "UnderMesh.ChecksPerSecond = 40  (map; default 20)"));
	CHECK(HasLine(lines, "UnderMesh.Remove = true  (default)"));
	CHECK(HasLine(lines, "Limit.Dodo_Character_BP_C = 300  (map; default 250)"));
	CHECK(HasLine(lines, "Limit.Lightbug_Character_BP_C = 400  (map)"));
	CHECK(HasLine(lines, "Limit.Lystro_Character_BP_C removed  (map; default 250)"));
	CHECK(HasLine(lines, "Overlap.Exclude = [\"Thrall\"]  (default)"));
	const std::vector<std::string> fallback = DescribeRules(settings.default_rules);
	CHECK(HasLine(fallback, "UnderMesh.ChecksPerSecond = 20  (default)"));
	CHECK(!AnyContains(fallback, "(map"));

	// A bad value in Default is reported once, as Default, not again for every map inheriting it.
	const Settings bad = ParseConfigText(R"({
		"Default": { "Overlap": { "Fraction": 5 } },
		"Maps": { "A": {}, "B": { "Overlap": { "Fraction": 7 } } }
	})");
	CHECK(AnyContains(bad.warnings, "Default.Overlap.Fraction"));
	CHECK(!AnyContains(bad.warnings, "Maps.A.Overlap"));
	CHECK(AnyContains(bad.warnings, "Maps.B.Overlap.Fraction"));   // its own bad value still counts

	const Settings misplaced = ParseConfigText(R"({ "Maps": { "Default": { "Overlap": {} } } })");
	CHECK(AnyContains(misplaced.warnings, "Maps.Default is read as a map named Default"));
	CHECK(!misplaced.has_default);

	const Settings unknown = ParseConfigText(R"({ "Default": { "Limits": {} } })");
	CHECK(AnyContains(unknown.warnings, "Default.Limits is not a known setting"));
}

static void WriteText(const std::filesystem::path& path, const std::string& text)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << text;
}

static void TestSharedConfig()
{
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "spawncontrol_shared_test";
	fs::remove_all(root);
	const fs::path plugin = root / "server" / "ArkApi" / "Plugins" / "SpawnControl";

	// No SharedConfig: the plugin's own file, as before.
	WriteText(plugin / "config.json", R"({ "General": { "RemovalsPerTick": 4 }, "Maps": { "Center": {} } })");
	Settings local = LoadConfigFiles(plugin);
	CHECK(local.removals_per_tick == 4 && local.maps.size() == 1 && local.sources.size() == 1);

	// A shared folder, given relative to the plugin folder.
	WriteText(plugin / "config.json", R"({ "SharedConfig": "../../../../shared", "General": { "RemovalsPerTick": 99 } })");
	WriteText(root / "shared" / "config.json", R"({ "General": { "RemovalsPerTick": 7 } })");
	WriteText(root / "shared" / "Default.json", R"({ "Overlap": { "Fraction": 0.5 }, "Limit": { "Dodo_Character_BP_C": 250 } })");
	WriteText(root / "shared" / "Aberration.json", R"({ "Limit": { "Dodo_Character_BP_C": 300, }, })");
	WriteText(root / "shared" / "Center.json", R"({ "UnderMesh": { "Remove": true } })");
	WriteText(root / "shared" / "notes.txt", "not json");
	Settings shared = LoadConfigFiles(plugin);
	CHECK(shared.removals_per_tick == 7);
	CHECK(shared.has_default && shared.maps.size() == 2);
	CHECK(shared.sources.size() == 4);
	CHECK(AnyContains(shared.warnings, "General ignored; with SharedConfig"));
	CHECK(AnyContains(shared.warnings, "Aberration.json line 1: comma"));
	const MapRules* aberration = FindMapRules(shared, "Aberration_WP");
	CHECK(aberration != nullptr && LimitOf(*aberration, "Dodo_Character_BP_C") == 300 && aberration->overlap.enabled);
	const MapRules* center = FindMapRules(shared, "Center");
	CHECK(center != nullptr && LimitOf(*center, "Dodo_Character_BP_C") == 250 && center->under_mesh.remove);

	// <Map>.auto.json: not a section of its own; its areas join that map's block areas, marked auto.
	WriteText(root / "shared" / "Center.auto.json", R"({ "BlockAreas": [ { "Name": "Auto 1 Dodo", "Location": [ 100, 200, 300 ], "RadiusInFoundations": 8 } ] })");
	WriteText(root / "shared" / "Nowhere.auto.json", R"({ "BlockAreas": [] })");
	Settings with_auto = LoadConfigFiles(plugin);
	CHECK(with_auto.maps.size() == 2 && !with_auto.auto_folder.empty());
	center = FindMapRules(with_auto, "Center");
	CHECK(center != nullptr && center->block_areas.size() == 1 && center->block_areas[0].is_auto &&
		center->block_areas[0].radius == 8 * kFoundation && center->block_areas[0].has_z);
	CHECK(AnyContains(with_auto.warnings, "Nowhere.auto.json: no Nowhere.json map file"));
	WriteText(root / "shared" / "Center.auto.json", R"({ "BlockAreas": [ )");
	with_auto = LoadConfigFiles(plugin);   // a broken auto file is a warning, never a failed load
	center = FindMapRules(with_auto, "Center");
	CHECK(center != nullptr && center->block_areas.empty() && AnyContains(with_auto.warnings, "Center.auto.json"));
	fs::remove(root / "shared" / "Center.auto.json");
	fs::remove(root / "shared" / "Nowhere.auto.json");
	CHECK(!LoadConfigFiles(plugin).auto_folder.empty());   // known even with no auto file yet, so the first can be written

	// A map file replaces the same section written in the shared config.json, with a warning.
	WriteText(root / "shared" / "config.json", R"({ "Maps": { "center": { "Limit": { "X_C": 1 } } } })");
	shared = LoadConfigFiles(plugin);
	CHECK(AnyContains(shared.warnings, "Center.json replaces the Maps.center section"));
	center = FindMapRules(shared, "Center");
	CHECK(center != nullptr && LimitOf(*center, "X_C") == -1);

	// A shared file, by absolute path.
	WriteText(root / "one.json", R"({ "SharedConfig": "elsewhere", "Maps": { "Island": {} } })");
	WriteText(plugin / "config.json", "{ \"SharedConfig\": " + nlohmann::json(SpawnControl::detail::Utf8FromPath(root / "one.json")).dump() + " }");
	Settings file = LoadConfigFiles(plugin);
	CHECK(file.maps.size() == 1 && file.sources.size() == 1);
	CHECK(AnyContains(file.warnings, "one.json: SharedConfig ignored"));

	// Missing or broken shared files throw, so a reload keeps what is in force.
	WriteText(plugin / "config.json", R"({ "SharedConfig": "../nowhere" })");
	CHECK_THROWS(LoadConfigFiles(plugin));
	WriteText(plugin / "config.json", R"({ "SharedConfig": "../../../../shared" })");
	WriteText(root / "shared" / "Island.json", R"({ "Limit": { )");
	CHECK_THROWS(LoadConfigFiles(plugin));
	WriteText(plugin / "config.json", R"({ "SharedConfig": 5 })");
	CHECK_THROWS(LoadConfigFiles(plugin));

	fs::remove_all(root);
}

static void TestAutoBlock()
{
	// Off unless enabled; values checked and reported under their own name.
	CHECK(!ParseConfigText(R"({ "Maps": { "A": {} } })").maps[0].auto_block.enabled);
	const Settings on = ParseConfigText(R"({ "Default": { "AutoBlockRepeats": { "Enabled": true, "MinRemoved": 6 } },
		"Maps": { "A": { "AutoBlockRepeats": { "MaxRadiusInFoundations": 12 } }, "B": { "AutoBlockRepeats": { "Enabled": false } } } })");
	CHECK(on.maps[0].auto_block.enabled && on.maps[0].auto_block.min_removed == 6 && on.maps[0].auto_block.max_radius_foundations == 12 &&
		on.maps[0].auto_block.max_areas == 50);
	CHECK(!on.maps[1].auto_block.enabled && on.maps[1].auto_block.min_removed == 6);
	const Settings bad = ParseConfigText(R"({ "Maps": { "A": { "AutoBlockRepeats": { "Enabled": "yes", "MinRemoved": 2, "MaxAreas": 0, "Radius": 3 } },
		"B": { "AutoBlockRepeats": true } } })");
	CHECK(!bad.maps[0].auto_block.enabled && bad.maps[0].auto_block.min_removed == 5 && bad.maps[0].auto_block.max_areas == 50);
	CHECK(AnyContains(bad.warnings, "Maps.A.AutoBlockRepeats.Enabled must be true or false"));
	CHECK(AnyContains(bad.warnings, "Maps.A.AutoBlockRepeats.MinRemoved must be at least 3"));
	CHECK(AnyContains(bad.warnings, "Maps.A.AutoBlockRepeats.MaxAreas must be at least 1"));
	CHECK(AnyContains(bad.warnings, "Maps.A.AutoBlockRepeats.Radius is not a known setting"));
	CHECK(AnyContains(bad.warnings, "Maps.B.AutoBlockRepeats must be an object"));

	// EnableZones: a list of zone names; a list replaces Default's like any other.
	const Settings zones = ParseConfigText(R"({ "Default": { "EnableZones": [ "Zone_A" ] },
		"Maps": { "A": {}, "B": { "EnableZones": [ "Zone_B", "Zone_C" ] }, "C": { "EnableZones": "Zone_D" } } })");
	CHECK(zones.maps[0].enable_zones == std::vector<std::string>{ "Zone_A" });
	CHECK((zones.maps[1].enable_zones == std::vector<std::string>{ "Zone_B", "Zone_C" }));
	CHECK(zones.maps[2].enable_zones.empty() && AnyContains(zones.warnings, "Maps.C.EnableZones must be a list"));   // replaced Default's, then ignored

	// Six removals on one spot become one area; disabled, full or too wide gives none.
	std::vector<RemovalPoint> points;
	for (int i = 0; i < 6; ++i)
		points.push_back({ 1000.0 + i * 50, 2000.0, -500.0, 3 });
	AutoBlockRule rule;
	rule.enabled = true;
	std::vector<RepeatSpot> spots = AutoBlockSpots(points, 10 * kFoundation, rule, {}, 0);
	CHECK(spots.size() == 1 && spots[0].count == 6 && spots[0].radius_foundations == 5);
	rule.min_removed = 7;
	CHECK(AutoBlockSpots(points, 10 * kFoundation, rule, {}, 0).empty());
	rule.min_removed = 5;
	rule.max_areas = 3;
	CHECK(AutoBlockSpots(points, 10 * kFoundation, rule, {}, 3).empty());
	BlockArea there;
	there.x = 1100;
	there.y = 2000;
	there.z = -500;
	there.has_z = true;
	there.radius = 5 * kFoundation;
	CHECK(AutoBlockSpots(points, 10 * kFoundation, rule, { there }, 0).empty());   // already blocked
	std::vector<RemovalPoint> wide;
	for (int i = 0; i < 6; ++i)
		wide.push_back({ i * 3.0 * kFoundation, 0.0, 0.0, 3 });   // 15 foundations end to end
	rule.max_radius_foundations = 5;
	CHECK(AutoBlockSpots(wide, 10 * kFoundation, rule, {}, 0).empty());
	rule.enabled = false;
	CHECK(AutoBlockSpots(points, 10 * kFoundation, rule, {}, 0).empty());

	// The saved file holds only the auto areas, rounded, in the BlockAreas format, and reads back the same.
	BlockArea pasted = there;
	pasted.name = "By hand";
	BlockArea automatic;
	automatic.name = "Auto 1 Dodo_Character_BP_C";
	automatic.x = 100.4;
	automatic.y = -200.6;
	automatic.z = 300.5;
	automatic.has_z = true;
	automatic.radius = 7 * kFoundation;
	automatic.is_auto = true;
	const nlohmann::json saved = AutoAreasJson({ pasted, automatic });
	CHECK(saved["BlockAreas"].size() == 1 && saved["BlockAreas"][0]["Name"] == "Auto 1 Dodo_Character_BP_C");
	CHECK(saved["BlockAreas"][0]["Location"] == nlohmann::json::array({ 100, -201, 301 }) && saved["BlockAreas"][0]["RadiusInFoundations"] == 7);
	std::vector<std::string> read_warnings;
	const std::vector<BlockArea> back = SpawnControl::detail::ReadAreas(saved["BlockAreas"], "x", read_warnings);
	CHECK(back.size() == 1 && read_warnings.empty() && back[0].radius == 7 * kFoundation && back[0].z == 301);
}

static void TestSpawnPoints()
{
	// Every way of writing a class names the same thing.
	for (const char* written : { "/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire", "/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire.Wyvern_Character_BP_Fire",
			 "/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire.Wyvern_Character_BP_Fire_C",
			 "Blueprint'/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire.Wyvern_Character_BP_Fire'" })
	{
		const SpawnClass c = SpawnClassFromPath(written, 2);
		CHECK(c.short_name == "Wyvern_Character_BP_Fire_C" && c.weight == 2);
		CHECK(c.blueprint == "Blueprint'/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire.Wyvern_Character_BP_Fire'");
	}

	// Weighted pick over [0, 1).
	const std::vector<SpawnClass> classes{ SpawnClassFromPath("/Game/A/One", 1), SpawnClassFromPath("/Game/A/Two", 3) };
	CHECK(PickWeighted(classes, 0.0) == 0 && PickWeighted(classes, 0.24) == 0 && PickWeighted(classes, 0.26) == 1 && PickWeighted(classes, 0.999) == 1);

	const Settings good = ParseConfigText(R"({ "Maps": { "R": { "SpawnPoints": [ { "Name": "Wyvern cave",
		"Classes": { "/Game/ScorchedEarth/Dinos/Wyvern/Wyvern_Character_BP_Fire": 1, "/Game/ASA/Dinos/IceWyvern/Wyvern_Character_BP_Ice": 2 },
		"Points": [ [ 5, -239538, 632 ], [ 100, 200, 300 ] ], "Target": 6, "AreaRadiusInFoundations": 150, "SpawnIntervalSeconds": 90,
		"BaseLevels": [ 10, 30 ] } ] } } })");
	CHECK(good.warnings.empty() && good.maps[0].spawn_points.size() == 1);
	const SpawnPointSet& set = good.maps[0].spawn_points[0];
	CHECK(set.name == "Wyvern cave" && set.classes.size() == 2 && set.points.size() == 2 && set.target == 6 &&
		set.area_radius == 150 * kFoundation && set.interval_seconds == 90 && set.min_base_level == 10 && set.max_base_level == 30);
	CHECK(set.points[0].y == -239538);   // Classes keep the JSON object's (sorted) key order: Ice first
	CHECK(set.classes[0].short_name == "Wyvern_Character_BP_Ice_C" && set.classes[0].weight == 2 && set.classes[1].short_name == "Wyvern_Character_BP_Fire_C");

	const Settings defaults = ParseConfigText(R"({ "Maps": { "R": { "SpawnPoints": [ { "Classes": { "/Game/A/One": 1 }, "Points": [ [ 0, 0, 0 ] ] } ] } } })");
	const SpawnPointSet& plain = defaults.maps[0].spawn_points[0];
	CHECK(plain.name == "Spawn points 1" && plain.target == 1 && plain.area_radius == 200 * kFoundation && plain.interval_seconds == 120 && plain.min_base_level == 0);

	const Settings bad = ParseConfigText(R"({ "Maps": { "R": { "SpawnPoints": [
		{ "Name": "No points", "Classes": { "/Game/A/One": 1 } },
		{ "Classes": { "/Game/A/One": 0, "/Game/A/Two": "x" }, "Points": [ [ 0, 0 ], [ 1, 2, 3 ] ] },
		{ "Classes": { "/Game/A/One": 1 }, "Points": [ [ 0, 0, 0 ] ], "Target": 0, "SpawnIntervalSeconds": 5, "BaseLevels": [ 30, 10 ], "Radius": 3 },
		7 ] }, "Q": { "SpawnPoints": {} } } })");
	const MapRules* r = FindMapRules(bad, "R");
	CHECK(r != nullptr && r->spawn_points.size() == 1 && r->spawn_points[0].target == 1 && r->spawn_points[0].interval_seconds == 120 &&
		r->spawn_points[0].min_base_level == 0);
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[0] (No points) needs at least one class"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[1].Classes./Game/A/One must be a weight above 0"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[1].Points has an entry that is not [ x, y, z ]"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[2].Target must be at least 1"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[2].SpawnIntervalSeconds must be at least 10"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[2].BaseLevels must be [ min, max ]"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[2].Radius is not a known setting"));
	CHECK(AnyContains(bad.warnings, "Maps.R.SpawnPoints[3] must be an object"));
	CHECK(AnyContains(bad.warnings, "Maps.Q.SpawnPoints must be a list"));
}

static void TestMeshZoneStop()
{
	CHECK(!MeshZoneShouldStop(4, 0));     // too few removals yet
	CHECK(MeshZoneShouldStop(5, 0));
	CHECK(MeshZoneShouldStop(5, 1));      // one clear creature in five removals
	CHECK(!MeshZoneShouldStop(5, 2));
	CHECK(!MeshZoneShouldStop(20, 5));    // a zone that also spawns where creatures belong keeps going
	CHECK(MeshZoneShouldStop(20, 4));

	const Settings on = ParseConfigText(R"({ "Maps": { "A": { "UnderMesh": { "Remove": true } } } })");
	CHECK(on.maps[0].under_mesh.stop_zones);   // on by default; acts only with Remove
	const Settings off = ParseConfigText(R"({ "Maps": { "A": { "UnderMesh": { "Remove": true, "StopZones": false } } } })");
	CHECK(!off.maps[0].under_mesh.stop_zones && off.warnings.empty());
	const Settings bad = ParseConfigText(R"({ "Maps": { "A": { "UnderMesh": { "StopZones": "yes" } } } })");
	CHECK(bad.maps[0].under_mesh.stop_zones && AnyContains(bad.warnings, "StopZones must be true or false; using true"));
}

static void TestChurnAndSources()
{
	ChurnWindow window;
	for (int i = 0; i < kChurnWindow - 1; ++i)
		window.Add(true);
	CHECK(!window.Churning());               // not a full window yet
	window.Add(true);
	CHECK(window.Churning() && window.Removed() == kChurnWindow);
	window.Add(false);
	window.Add(false);
	CHECK(window.Churning(18) && window.Removed() == kChurnWindow - 2);   // 18 of the last 20
	window.Add(false);
	CHECK(!window.Churning(18) && window.Churning());   // 17 of 20: under a strict 18, over the default 12
	for (int i = 0; i < kChurnWindow; ++i)
		window.Add(i % 2 == 0);
	CHECK(!window.Churning() && window.Removed() == kChurnWindow / 2 && window.count == kChurnWindow);   // a mixed group entry at 50%
	ChurnWindow mixed;
	for (int i = 0; i < kChurnWindow; ++i)
		mixed.Add(i < kChurnRemoved);
	CHECK(mixed.Churning() && mixed.Removed() == kChurnRemoved);   // 12 of 20: held by default
	CHECK(!mixed.Churning(13));

	CHECK(ParseConfigText(R"({})").churn_removed == kChurnRemoved);
	CHECK(ParseConfigText(R"({ "General": { "ChurnRemoved": 16 } })").churn_removed == 16);
	const Settings too_high = ParseConfigText(R"({ "General": { "ChurnRemoved": 21 } })");
	CHECK(too_high.churn_removed == kChurnRemoved && AnyContains(too_high.warnings, "at most 20"));
	const Settings too_low = ParseConfigText(R"({ "General": { "ChurnRemoved": 0 } })");
	CHECK(too_low.churn_removed == kChurnRemoved && AnyContains(too_low.warnings, "ChurnRemoved must be at least 1"));

	CHECK(SummarizeSources({ "D:\\a\\config.json" }) == "D:\\a\\config.json");
	CHECK(SummarizeSources({ "D:\\s\\config.json", "D:\\s\\Aberration.json", "D:\\s\\Default.json", "D:\\s\\Island.json" }) ==
		"D:\\s (config.json, Default.json, 2 map files)");
	CHECK(SummarizeSources({ "/srv/s/Center.json", "/srv/s/Island.json" }) == "/srv/s (2 map files)");

	const Settings same = ParseConfigText(R"({
		"Default": { "Overlap": { "Fraction": 0.5, "Exclude": [ "A_C" ] }, "BlockAreas": [ { "Name": "x", "Location": [ 1, 2 ], "RadiusInFoundations": 1 } ] },
		"Maps": { "M": { "Overlap": { "Fraction": 0.5, "Exclude": [ "B_C" ] },
			"BlockAreas": [ { "Name": "y", "Location": [ 1, 2 ], "RadiusInFoundations": 1 } ] } }
	})");
	const std::vector<std::string> lines = DescribeRules(same.maps[0]);
	CHECK(HasLine(lines, "Overlap.Fraction = 0.5  (map, same as default)"));
	CHECK(HasLine(lines, "Overlap.Exclude = [\"B_C\"]  (map; default [\"A_C\"])"));
	CHECK(AnyContains(lines, "BlockAreas[0] = ") && AnyContains(lines, "(map; replaces the default list)"));
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
	TestOverlap();
	TestConfigChecks();
	TestUnderMesh();
	TestRepeatSpots();
	TestOverOnSpawn();
	TestIsFull();
	TestRefresh();
	TestDefaultSection();
	TestSharedConfig();
	TestAutoBlock();
	TestSpawnPoints();
	TestMeshZoneStop();
	TestChurnAndSources();

	if (argc > 1)
		TestConfigFile(argv[1], argc > 2 && std::string(argv[2]) == "--shipped");

	std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
