#pragma once

// Pure logic for SpawnControl: config parsing, class rules (block, limit, group limit, protection),
// and the plan of which wild creatures to remove.
//
// No dependency on the game or on server API types (only on the nlohmann json.hpp that ships with
// AsaApi and AseApi), so it compiles and unit-tests on any machine. Identical in the ASA and ASE
// folders; keep the two copies in sync. See tests/logic_tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json.hpp"

namespace SpawnControl
{
	// Player and tribe teams start at 50000; anything below belongs to the world.
	constexpr int kFirstPlayerTeam = 50000;

	// ------------------------------------------------------------------------------------------
	// Strings
	// ------------------------------------------------------------------------------------------

	inline std::string ToLower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out)
			c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
		return out;
	}

	inline bool EqualsNoCase(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && ToLower(a) == ToLower(b);
	}

	inline bool ContainsNoCase(std::string_view text, std::string_view part)
	{
		return !part.empty() && ToLower(text).find(ToLower(part)) != std::string::npos;
	}

	// Turns a blueprint reference into the class path ListDinos prints:
	//   Blueprint'/Game/A/Otter_BP.Otter_BP'  ->  /Game/A/Otter_BP.Otter_BP_C
	// A path that already ends in _C, or has no package dot, is returned trimmed but unchanged.
	inline std::string ClassPathFromReference(std::string_view reference)
	{
		std::string path(reference);
		const size_t open = path.find('\'');
		if (open != std::string::npos)
		{
			path = path.substr(open + 1);
			const size_t close = path.rfind('\'');
			if (close != std::string::npos)
				path.resize(close);
		}
		if (path.find('.') == std::string::npos)
			return path;
		if (path.size() >= 2 && path.compare(path.size() - 2, 2, "_C") == 0)
			return path;
		return path + "_C";
	}

	// The part after the last '.', or the whole text: /Game/A/Otter_BP.Otter_BP_C -> Otter_BP_C
	inline std::string ShortNameFromPath(std::string_view path)
	{
		const size_t dot = path.rfind('.');
		return std::string(dot == std::string_view::npos ? path : path.substr(dot + 1));
	}

	// A config key with a '/' is a full class path; anything else is a short class name.
	inline bool IsPathKey(std::string_view key)
	{
		return key.find('/') != std::string_view::npos;
	}

	// ------------------------------------------------------------------------------------------
	// Classes
	// ------------------------------------------------------------------------------------------

	struct ClassInfo
	{
		std::string short_name;          // Otter_Character_BP_C
		std::string path;                // /Game/PrimalEarth/Dinos/Otter/Otter_Character_BP.Otter_Character_BP_C
		std::vector<std::string> chain;  // own class first, then every parent
	};

	inline bool KeyMatches(std::string_view key, const ClassInfo& info)
	{
		return IsPathKey(key) ? EqualsNoCase(key, info.path) : EqualsNoCase(key, info.short_name);
	}

	// ------------------------------------------------------------------------------------------
	// Settings
	// ------------------------------------------------------------------------------------------

	struct GroupRule
	{
		std::string name;
		int max = 0;
		std::vector<std::string> classes;
	};

	constexpr double kFoundation = 300.0;      // one foundation in Unreal units
	constexpr int kMaxDensityArea = 200;       // foundations

	// A clump cap: no more than max of these classes in any cube size across.
	struct DensityRule
	{
		std::string name;                  // the key under Density
		int max = 0;
		double size = 0;                   // Unreal units
		bool all = false;                  // "*": every wild creature that is not protected
		std::vector<std::string> classes;  // counted together; the key itself when Classes is not given
		std::vector<std::string> exclude;  // never counted: a short name also matches derived classes, a path part any path containing it
	};

	// Places where density rules do not apply: a horizontal circle around a point.
	struct DensityArea
	{
		std::string name;
		double x = 0, y = 0;
		double radius = 0;                 // Unreal units
	};

	// Where no wild creature belongs, e.g. under a misplaced spawner: everything wild inside is removed and
	// zones that only spawn inside are stopped. A sphere when the location has a height, else a column.
	struct BlockArea
	{
		std::string name;
		double x = 0, y = 0, z = 0;
		bool has_z = false;
		double radius = 0;                 // Unreal units
	};

	constexpr int kZoneStopAfter = 3;      // spawns inside a block area, none outside, before a zone is stopped

	inline int BlockAreaAt(const std::vector<BlockArea>& areas, double x, double y, double z)
	{
		for (size_t a = 0; a < areas.size(); ++a)
		{
			const BlockArea& area = areas[a];
			const double dx = x - area.x, dy = y - area.y, dz = area.has_z ? z - area.z : 0.0;
			if (dx * dx + dy * dy + dz * dz <= area.radius * area.radius)
				return static_cast<int>(a);
		}
		return -1;
	}

	// A zone is stopped once it has put several creatures inside block areas and none anywhere else, so a
	// zone that also spawns in normal places keeps working.
	inline bool ZoneShouldStop(int spawned_inside, int spawned_outside)
	{
		return spawned_inside >= kZoneStopAfter && spawned_outside == 0;
	}

	// Creatures spawned inside one another: two overlap when their centres are closer than Fraction of
	// their summed collision radii. One of each overlapping set stays.
	struct OverlapRule
	{
		bool enabled = false;
		double fraction = 0.5;
		std::vector<std::string> exclude;
	};

	struct DensitySkip
	{
		bool caves = true;                 // creatures the game spawned in a cave or boss area
		std::vector<DensityArea> areas;
	};

	struct MapRules
	{
		std::string name;                               // section name as written in config.json
		std::vector<std::string> block;
		std::vector<std::pair<std::string, int>> limits; // class key -> max wild count
		std::vector<GroupRule> groups;
		std::vector<DensityRule> density;
		DensitySkip density_skip;
		std::vector<BlockArea> block_areas;
		OverlapRule overlap;
	};

	struct ReportFamily
	{
		std::string label;       // shown in List, e.g. "Thralls"
		std::string base_class;  // a parent class name every member derives from
	};

	struct Settings
	{
		int removals_per_tick = 10;
		int census_actors_per_tick = 5000;
		int census_interval_seconds = 60;
		double near_player_distance = 10000.0; // creatures closer than this to a player are removed last
		bool log_removals = true;
		bool refuse_full_entries = true;       // zones skip spawn entries whose creatures are all at their limit
		bool ignore_summoned = true;           // creatures another creature summoned (escorts) are outside the rules

		std::vector<std::string> protected_paths{ "/Missions/" };
		std::vector<std::string> protected_families;
		std::vector<ReportFamily> report_families;

		std::vector<MapRules> maps;
		std::vector<std::string> warnings;

		// Rolling refresh: wild creatures replaced a few at a time so the zones respawn them with fresh
		// levels, instead of a full wipe. 0 hours = off.
		int refresh_hours = 0;                   // the whole wild population turns over in about this long
		int refresh_min_age_minutes = 60;        // never refresh a creature first seen less than this ago
		double refresh_player_distance = 30000;  // never refresh a creature this close to a player
		int refresh_recent_damage_seconds = 300; // never refresh a creature that took damage this recently
		std::vector<std::string> refresh_skip_paths{ "/Missions/", "Thrall", "Boss" };
	};

	namespace detail
	{
		inline int ReadInt(const nlohmann::json& object, const char* key, int fallback, int minimum,
			std::vector<std::string>& warnings)
		{
			if (!object.is_object() || !object.contains(key))
				return fallback;
			const nlohmann::json& value = object[key];
			if (!value.is_number_integer())
			{
				warnings.push_back(std::string(key) + " must be a whole number; using " + std::to_string(fallback));
				return fallback;
			}
			const int number = value.get<int>();
			if (number < minimum)
			{
				warnings.push_back(std::string(key) + " must be at least " + std::to_string(minimum) + "; using " +
					std::to_string(fallback));
				return fallback;
			}
			return number;
		}

		inline std::vector<std::string> ReadStrings(const nlohmann::json& object, const char* key,
			std::vector<std::string> fallback, std::vector<std::string>& warnings, const std::string& where)
		{
			if (!object.is_object() || !object.contains(key))
				return fallback;
			const nlohmann::json& value = object[key];
			if (!value.is_array())
			{
				warnings.push_back(where + key + " must be a list of strings; ignored");
				return fallback;
			}
			std::vector<std::string> out;
			for (const nlohmann::json& item : value)
			{
				if (item.is_string() && !item.get<std::string>().empty())
					out.push_back(item.get<std::string>());
				else
					warnings.push_back(where + key + " has an entry that is not a class name; skipped");
			}
			return out;
		}

		// A list of { "Name", "Location": [x, y] or [x, y, z], "RadiusInFoundations" }.
		inline std::vector<BlockArea> ReadAreas(const nlohmann::json& areas, const std::string& where,
			std::vector<std::string>& warnings)
		{
			std::vector<BlockArea> out;
			if (!areas.is_array())
			{
				warnings.push_back(where + " must be a list; ignored");
				return out;
			}
			for (size_t a = 0; a < areas.size(); ++a)
			{
				const nlohmann::json& entry = areas[a];
				const std::string area_where = where + "[" + std::to_string(a) + "] ";
				const nlohmann::json* location = entry.is_object() && entry.contains("Location") ? &entry["Location"] : nullptr;
				const int radius = ReadInt(entry, "RadiusInFoundations", -1, 1, warnings);
				if (location == nullptr || !location->is_array() || location->size() < 2 || location->size() > 3 ||
					!(*location)[0].is_number() || !(*location)[1].is_number() ||
					(location->size() == 3 && !(*location)[2].is_number()) || radius < 1)
				{
					warnings.push_back(area_where + "needs Location [x, y] or [x, y, z] and RadiusInFoundations of 1 or more; skipped");
					continue;
				}
				BlockArea area;
				area.name = entry.contains("Name") && entry["Name"].is_string() ? entry["Name"].get<std::string>() : "";
				area.x = (*location)[0].get<double>();
				area.y = (*location)[1].get<double>();
				area.has_z = location->size() == 3;
				area.z = area.has_z ? (*location)[2].get<double>() : 0.0;
				area.radius = radius * kFoundation;
				out.push_back(std::move(area));
			}
			return out;
		}

		inline MapRules ReadMap(const std::string& name, const nlohmann::json& section, std::vector<std::string>& warnings)
		{
			MapRules rules;
			rules.name = name;
			const std::string where = "Maps." + name + ".";
			if (!section.is_object())
			{
				warnings.push_back("Maps." + name + " must be an object; ignored");
				return rules;
			}

			rules.block = ReadStrings(section, "Block", {}, warnings, where);

			if (section.contains("Limit"))
			{
				const nlohmann::json& limits = section["Limit"];
				if (!limits.is_object())
					warnings.push_back(where + "Limit must be an object of class -> number; ignored");
				else
				{
					for (auto it = limits.begin(); it != limits.end(); ++it)
					{
						if (!it.value().is_number_integer() || it.value().get<int>() < 0)
						{
							warnings.push_back(where + "Limit." + it.key() + " must be a whole number of 0 or more; skipped");
							continue;
						}
						const int max = it.value().get<int>();
						if (max == 0)
							continue; // 0 = no limit, the same as leaving the class out
						rules.limits.emplace_back(it.key(), max);
					}
				}
			}

			if (section.contains("GroupLimit"))
			{
				const nlohmann::json& groups = section["GroupLimit"];
				if (!groups.is_object())
					warnings.push_back(where + "GroupLimit must be an object of name -> group; ignored");
				else
				{
					for (auto it = groups.begin(); it != groups.end(); ++it)
					{
						const std::string group_where = where + "GroupLimit." + it.key() + ".";
						GroupRule group;
						group.name = it.key();
						group.max = ReadInt(it.value(), "Max", -1, 1, warnings);
						group.classes = ReadStrings(it.value(), "Classes", {}, warnings, group_where);
						if (group.max < 1)
						{
							warnings.push_back(group_where + "Max is missing or below 1; group skipped");
							continue;
						}
						if (group.classes.size() < 2)
						{
							warnings.push_back(group_where + "Classes needs at least two classes; use Limit for one");
							if (group.classes.empty())
								continue;
						}
						rules.groups.push_back(std::move(group));
					}
				}
			}

			if (section.contains("Density"))
			{
				const nlohmann::json& density = section["Density"];
				if (!density.is_object())
					warnings.push_back(where + "Density must be an object of class or name -> { Max, AreaInFoundations }; ignored");
				else
				{
					for (auto it = density.begin(); it != density.end(); ++it)
					{
						const std::string density_where = where + "Density." + it.key() + ".";
						if (!it.value().is_object())
						{
							warnings.push_back(where + "Density." + it.key() + " must be an object with Max and AreaInFoundations; skipped");
							continue;
						}
						DensityRule rule;
						rule.name = it.key();
						rule.all = it.key() == "*";
						rule.max = ReadInt(it.value(), "Max", -1, 1, warnings);
						int area = ReadInt(it.value(), "AreaInFoundations", -1, 1, warnings);
						rule.classes = ReadStrings(it.value(), "Classes", {}, warnings, density_where);
						rule.exclude = ReadStrings(it.value(), "Exclude", {}, warnings, density_where);
						if (rule.max < 1 || area < 1)
						{
							warnings.push_back(density_where + "Max and AreaInFoundations must both be 1 or more; skipped");
							continue;
						}
						if (area > kMaxDensityArea)
						{
							warnings.push_back(density_where + "AreaInFoundations above " + std::to_string(kMaxDensityArea) +
								"; using " + std::to_string(kMaxDensityArea));
							area = kMaxDensityArea;
						}
						if (!rule.all && rule.classes.empty())
							rule.classes.push_back(it.key());
						rule.size = area * kFoundation;
						rules.density.push_back(std::move(rule));
					}
				}
			}

			if (section.contains("DensitySkip"))
			{
				const nlohmann::json& skip = section["DensitySkip"];
				const std::string skip_where = where + "DensitySkip.";
				if (!skip.is_object())
					warnings.push_back(where + "DensitySkip must be an object with Caves and Areas; ignored");
				else
				{
					if (skip.contains("Caves"))
					{
						if (skip["Caves"].is_boolean())
							rules.density_skip.caves = skip["Caves"].get<bool>();
						else
							warnings.push_back(skip_where + "Caves must be true or false; using true");
					}
					if (skip.contains("Areas"))
					{
						for (const BlockArea& area : ReadAreas(skip["Areas"], skip_where + "Areas", warnings))
							rules.density_skip.areas.push_back({ area.name, area.x, area.y, area.radius });
					}
				}
			}

			if (section.contains("Overlap"))
			{
				const nlohmann::json& overlap = section["Overlap"];
				const std::string overlap_where = where + "Overlap.";
				if (!overlap.is_object())
					warnings.push_back(where + "Overlap must be an object with Fraction and Exclude; ignored");
				else
				{
					rules.overlap.enabled = true;
					if (overlap.contains("Fraction"))
					{
						const nlohmann::json& fraction = overlap["Fraction"];
						if (fraction.is_number() && fraction.get<double>() >= 0.1 && fraction.get<double>() <= 1.0)
							rules.overlap.fraction = fraction.get<double>();
						else
							warnings.push_back(overlap_where + "Fraction must be a number from 0.1 to 1; using 0.5");
					}
					rules.overlap.exclude = ReadStrings(overlap, "Exclude", {}, warnings, overlap_where);
				}
			}

			if (section.contains("BlockAreas"))
				rules.block_areas = ReadAreas(section["BlockAreas"], where + "BlockAreas", warnings);
			return rules;
		}
	}

	namespace detail
	{
		inline bool OneOf(const std::string& key, std::initializer_list<const char*> known)
		{
			for (const char* name : known)
			{
				if (key == name)
					return true;
			}
			return false;
		}

		constexpr std::initializer_list<const char*> kMapKeys = { "Block", "Limit", "GroupLimit", "Density", "DensitySkip", "BlockAreas", "Overlap" };

		// Keys nobody reads are ignored by ARK-style configs without a word; here each one is a warning, with
		// a hint when the key exists one level up or down.
		inline void WarnUnknown(const nlohmann::json& object, std::initializer_list<const char*> known, const std::string& where,
			std::vector<std::string>& warnings)
		{
			if (!object.is_object())
				return;
			for (auto it = object.begin(); it != object.end(); ++it)
			{
				if (OneOf(it.key(), known))
					continue;
				std::string hint;
				if (OneOf(it.key(), kMapKeys))
					hint = " (it belongs directly inside a map section, next to Limit)";
				warnings.push_back(where + it.key() + " is not a known setting here; ignored" + hint);
			}
		}

		inline void WarnUnknownAreas(const nlohmann::json& areas, const std::string& where, std::vector<std::string>& warnings)
		{
			if (!areas.is_array())
				return;
			for (size_t a = 0; a < areas.size(); ++a)
				WarnUnknown(areas[a], { "Name", "Location", "RadiusInFoundations" }, where + "[" + std::to_string(a) + "].", warnings);
		}

		inline void WarnUnknownKeys(const nlohmann::json& root, std::vector<std::string>& warnings)
		{
			WarnUnknown(root, { "General", "Protect", "Refresh", "ReportFamilies", "Maps" }, "", warnings);
			if (root.contains("General"))
				WarnUnknown(root["General"], { "RemovalsPerTick", "CensusActorsPerTick", "CensusIntervalSeconds", "NearPlayerDistance",
					"LogRemovals", "RefuseFullEntries", "IgnoreSummoned" }, "General.", warnings);
			if (root.contains("Protect"))
				WarnUnknown(root["Protect"], { "PathContains", "Families" }, "Protect.", warnings);
			if (root.contains("Refresh"))
				WarnUnknown(root["Refresh"], { "TurnoverHours", "MinAgeMinutes", "PlayerDistance", "RecentDamageSeconds",
					"SkipPathContains" }, "Refresh.", warnings);
			if (!root.contains("Maps") || !root["Maps"].is_object())
				return;
			const nlohmann::json& maps = root["Maps"];
			for (auto it = maps.begin(); it != maps.end(); ++it)
			{
				const std::string where = "Maps." + it.key() + ".";
				if (OneOf(it.key(), kMapKeys))
				{
					warnings.push_back("Maps." + it.key() + " is a rule, not a map; it belongs inside a map section such as Maps.<map>." +
						it.key() + ", so it is not applied to any map");
					continue;
				}
				const nlohmann::json& map = it.value();
				WarnUnknown(map, kMapKeys, where, warnings);
				if (!map.is_object())
					continue;
				if (map.contains("GroupLimit") && map["GroupLimit"].is_object())
				{
					for (auto g = map["GroupLimit"].begin(); g != map["GroupLimit"].end(); ++g)
						WarnUnknown(g.value(), { "Max", "Classes" }, where + "GroupLimit." + g.key() + ".", warnings);
				}
				if (map.contains("Density") && map["Density"].is_object())
				{
					for (auto d = map["Density"].begin(); d != map["Density"].end(); ++d)
						WarnUnknown(d.value(), { "Max", "AreaInFoundations", "Classes", "Exclude" }, where + "Density." + d.key() + ".", warnings);
				}
				if (map.contains("DensitySkip"))
				{
					WarnUnknown(map["DensitySkip"], { "Caves", "Areas" }, where + "DensitySkip.", warnings);
					if (map["DensitySkip"].is_object() && map["DensitySkip"].contains("Areas"))
						WarnUnknownAreas(map["DensitySkip"]["Areas"], where + "DensitySkip.Areas", warnings);
				}
				if (map.contains("BlockAreas"))
					WarnUnknownAreas(map["BlockAreas"], where + "BlockAreas", warnings);
				if (map.contains("Overlap"))
					WarnUnknown(map["Overlap"], { "Fraction", "Exclude" }, where + "Overlap.", warnings);
			}
		}
	}

	// Throws only for a document that is not an object; everything else becomes a warning and a default.
	inline Settings ParseSettings(const nlohmann::json& root)
	{
		if (!root.is_object())
			throw std::runtime_error("config.json must be a JSON object");

		Settings settings;
		std::vector<std::string>& warnings = settings.warnings;
		detail::WarnUnknownKeys(root, warnings);

		const nlohmann::json general = root.value("General", nlohmann::json::object());
		settings.removals_per_tick = detail::ReadInt(general, "RemovalsPerTick", settings.removals_per_tick, 1, warnings);
		settings.census_actors_per_tick = detail::ReadInt(general, "CensusActorsPerTick", settings.census_actors_per_tick, 100, warnings);
		settings.census_interval_seconds = detail::ReadInt(general, "CensusIntervalSeconds", settings.census_interval_seconds, 5, warnings);
		settings.near_player_distance = detail::ReadInt(general, "NearPlayerDistance", static_cast<int>(settings.near_player_distance), 0, warnings);
		for (const auto& [key, target] : { std::pair<const char*, bool*>{ "LogRemovals", &settings.log_removals },
				std::pair<const char*, bool*>{ "RefuseFullEntries", &settings.refuse_full_entries },
				std::pair<const char*, bool*>{ "IgnoreSummoned", &settings.ignore_summoned } })
		{
			if (!general.is_object() || !general.contains(key))
				continue;
			if (general[key].is_boolean())
				*target = general[key].get<bool>();
			else
				warnings.push_back(std::string("General.") + key + " must be true or false");
		}

		const nlohmann::json protect = root.value("Protect", nlohmann::json::object());
		settings.protected_paths = detail::ReadStrings(protect, "PathContains", settings.protected_paths, warnings, "Protect.");
		settings.protected_families = detail::ReadStrings(protect, "Families", {}, warnings, "Protect.");

		if (root.contains("ReportFamilies"))
		{
			const nlohmann::json& families = root["ReportFamilies"];
			if (!families.is_object())
				warnings.push_back("ReportFamilies must be an object of label -> base class; ignored");
			else
			{
				for (auto it = families.begin(); it != families.end(); ++it)
				{
					if (it.value().is_string() && !it.value().get<std::string>().empty())
						settings.report_families.push_back({ it.key(), it.value().get<std::string>() });
					else
						warnings.push_back("ReportFamilies." + it.key() + " must be a class name; skipped");
				}
			}
		}

		const nlohmann::json refresh = root.value("Refresh", nlohmann::json::object());
		settings.refresh_hours = detail::ReadInt(refresh, "TurnoverHours", settings.refresh_hours, 0, warnings);
		settings.refresh_min_age_minutes = detail::ReadInt(refresh, "MinAgeMinutes", settings.refresh_min_age_minutes, 0, warnings);
		settings.refresh_player_distance = detail::ReadInt(refresh, "PlayerDistance", static_cast<int>(settings.refresh_player_distance), 0, warnings);
		settings.refresh_recent_damage_seconds = detail::ReadInt(refresh, "RecentDamageSeconds", settings.refresh_recent_damage_seconds, 0, warnings);
		settings.refresh_skip_paths = detail::ReadStrings(refresh, "SkipPathContains", settings.refresh_skip_paths, warnings, "Refresh.");

		if (root.contains("Maps"))
		{
			const nlohmann::json& maps = root["Maps"];
			if (!maps.is_object())
				warnings.push_back("Maps must be an object of map name -> rules; ignored");
			else
			{
				for (auto it = maps.begin(); it != maps.end(); ++it)
					settings.maps.push_back(detail::ReadMap(it.key(), it.value(), warnings));
			}
		}
		return settings;
	}

	// Picks the section for a map: an exact (case-insensitive) name first, then the name without a
	// trailing "_WP" (ASA's world-partition map names). Returns nullptr when no section fits.
	inline const MapRules* FindMapRules(const Settings& settings, std::string_view map_name)
	{
		for (const MapRules& rules : settings.maps)
		{
			if (EqualsNoCase(rules.name, map_name))
				return &rules;
		}
		std::string trimmed(map_name);
		if (trimmed.size() > 3 && EqualsNoCase(std::string_view(trimmed).substr(trimmed.size() - 3), "_WP"))
		{
			trimmed.resize(trimmed.size() - 3);
			for (const MapRules& rules : settings.maps)
			{
				if (EqualsNoCase(rules.name, trimmed))
					return &rules;
			}
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Class rules
	// ------------------------------------------------------------------------------------------

	constexpr int kNoLimit = -1;

	struct ClassRule
	{
		bool is_protected = false;
		bool blocked = false;
		int limit = kNoLimit;
		std::vector<int> groups;   // indices into MapRules::groups
		std::vector<int> density;  // indices into MapRules::density
		bool overlap = false;      // checked for spawning inside other creatures
		int report_family = -1;    // index into Settings::report_families
	};

	inline bool ChainContains(const ClassInfo& info, std::string_view name)
	{
		for (const std::string& link : info.chain)
		{
			if (EqualsNoCase(link, name))
				return true;
		}
		return false;
	}

	// Resolves every rule for one class. A full-path Limit beats a short-name Limit for the same class,
	// so one same-named class from a mod can be treated differently. rules may be nullptr (no section).
	// An Exclude list: a short name matches the class or any class derived from it, an entry starting with
	// "/" any class whose path contains it.
	inline bool ExcludedBy(const std::vector<std::string>& keys, const ClassInfo& info)
	{
		for (const std::string& key : keys)
		{
			if (IsPathKey(key) ? ContainsNoCase(info.path, key) : KeyMatches(key, info) || ChainContains(info, key))
				return true;
		}
		return false;
	}

	inline ClassRule ResolveClass(const Settings& settings, const MapRules* rules, const ClassInfo& info)
	{
		ClassRule rule;

		for (const std::string& part : settings.protected_paths)
			rule.is_protected = rule.is_protected || ContainsNoCase(info.path, part);
		for (const std::string& family : settings.protected_families)
			rule.is_protected = rule.is_protected || ChainContains(info, family);

		for (size_t i = 0; i < settings.report_families.size() && rule.report_family < 0; ++i)
		{
			if (ChainContains(info, settings.report_families[i].base_class))
				rule.report_family = static_cast<int>(i);
		}

		if (rules == nullptr || rule.is_protected)
			return rule;

		for (const std::string& key : rules->block)
			rule.blocked = rule.blocked || KeyMatches(key, info);

		int short_limit = kNoLimit;
		int path_limit = kNoLimit;
		for (const auto& [key, max] : rules->limits)
		{
			if (!KeyMatches(key, info))
				continue;
			if (IsPathKey(key))
				path_limit = max;
			else
				short_limit = max;
		}
		rule.limit = path_limit != kNoLimit ? path_limit : short_limit;

		for (size_t g = 0; g < rules->groups.size(); ++g)
		{
			for (const std::string& key : rules->groups[g].classes)
			{
				if (KeyMatches(key, info))
				{
					rule.groups.push_back(static_cast<int>(g));
					break;
				}
			}
		}

		for (size_t d = 0; d < rules->density.size(); ++d)
		{
			bool member = rules->density[d].all;
			for (const std::string& key : rules->density[d].classes)
				member = member || KeyMatches(key, info);
			if (member && !ExcludedBy(rules->density[d].exclude, info))
				rule.density.push_back(static_cast<int>(d));
		}

		rule.overlap = rules->overlap.enabled && !ExcludedBy(rules->overlap.exclude, info);
		return rule;
	}

	inline bool HasAnyRule(const ClassRule& rule)
	{
		return rule.blocked || rule.limit != kNoLimit || !rule.groups.empty() || !rule.density.empty() || rule.overlap;
	}

	// ------------------------------------------------------------------------------------------
	// Removal plan
	// ------------------------------------------------------------------------------------------

	// One wild, unowned, non-mission creature seen by a census pass.
	struct Creature
	{
		size_t id = 0;               // caller's handle (index into its own list)
		int class_index = 0;         // index into the rules vector passed to PlanRemovals
		double player_distance = 0;  // to the nearest player; larger = removed sooner
		double x = 0, y = 0, z = 0;  // where it is, for density rules
		bool in_cave = false;        // the game spawned it in a cave or boss area
		bool in_block_area = false;  // inside a block area: removed whatever its class
		double radius = 0;           // collision radius; 0 = unknown, never counted as overlapping
		bool attached = false;       // riding or carried: sits inside another creature on purpose
	};

	struct OverlapSample
	{
		double x = 0, y = 0, z = 0;
		int removed_class = -1;
		int kept_class = -1;
	};
	constexpr size_t kOverlapSamples = 10;

	inline bool SkipsDensity(const DensitySkip& skip, const Creature& creature)
	{
		if (skip.caves && creature.in_cave)
			return true;
		for (const DensityArea& area : skip.areas)
		{
			const double dx = creature.x - area.x, dy = creature.y - area.y;
			if (dx * dx + dy * dy <= area.radius * area.radius)
				return true;
		}
		return false;
	}

	constexpr size_t kHotspotsPerRule = 10;

	// One of the fullest cubes a density rule found before thinning, for tuning Max and finding dumps.
	struct DensityHotspot
	{
		int count = 0;
		double x = 0, y = 0, z = 0;          // average position of the creatures in it
		std::vector<std::pair<int, int>> classes;  // (count, class index), largest first
	};

	struct Plan
	{
		std::vector<size_t> remove;               // ids, in the order to remove them
		std::vector<int> class_counts;            // wild count per class index
		std::vector<int> group_counts;            // wild count per group
		size_t blocked = 0;
		size_t over_limit = 0;
		size_t over_group = 0;
		size_t too_dense = 0;
		size_t in_block_area = 0;
		size_t overlapping = 0;
		std::vector<OverlapSample> overlap_samples;  // a few of them, for finding where it happens
		std::vector<std::vector<DensityHotspot>> hotspots;  // per density rule, fullest first
	};

	// Which creatures to remove so every block, limit and group limit holds. Blocked creatures come
	// first; within each rule the creatures farthest from any player go first, so removals happen where
	// nobody sees them. A creature is only ever picked once. Group limits take from whichever member
	// class has the most left, so 250 vanilla + 250 modded otters capped at 200 end near 100 + 100.
	//
	// Density rules run last, on what the other rules leave: no cube AreaInFoundations across may hold
	// more than Max of a rule's classes. Checked on two grids, the second shifted by half a cube, so a
	// clump on a cube edge is still caught; the farthest from players in an over-full cube go first.
	inline Plan PlanRemovals(const std::vector<ClassRule>& rules, const std::vector<GroupRule>& groups,
		const std::vector<Creature>& creatures, const std::vector<DensityRule>& density = {}, const DensitySkip& skip = {},
		double overlap_fraction = 0.5)
	{
		Plan plan;
		plan.class_counts.assign(rules.size(), 0);
		plan.group_counts.assign(groups.size(), 0);

		auto valid_group = [&](int g) { return g >= 0 && static_cast<size_t>(g) < groups.size(); };

		// Per class: what is still unpicked, farthest from players first (ties keep census order).
		// Creatures inside a block area go first, whatever their class, and are not counted.
		std::vector<bool> removed(creatures.size(), false);

		// Overlap: walk from the creature nearest a player outwards; one that sits inside a creature already
		// kept goes, so of each stack the one most likely in view stays and the rest are removed.
		{
			std::vector<size_t> order;
			double largest = 0;
			for (size_t i = 0; i < creatures.size(); ++i)
			{
				const Creature& c = creatures[i];
				if (c.class_index < 0 || static_cast<size_t>(c.class_index) >= rules.size() || rules[c.class_index].is_protected ||
					!rules[c.class_index].overlap || c.in_block_area || c.attached || c.radius <= 0)
					continue;
				order.push_back(i);
				largest = (std::max)(largest, c.radius);
			}
			if (order.size() >= 2)
			{
				// Overlapping pairs, found by sweeping along x: only creatures within reach on x can overlap.
				std::vector<size_t> by_x(order.size());
				for (size_t k = 0; k < order.size(); ++k)
					by_x[k] = k;
				std::sort(by_x.begin(), by_x.end(), [&](size_t a, size_t b) { return creatures[order[a]].x < creatures[order[b]].x; });
				const double reach = 2 * largest * overlap_fraction;
				std::vector<std::vector<size_t>> touching(order.size());
				for (size_t a = 0; a < by_x.size(); ++a)
				{
					const Creature& c = creatures[order[by_x[a]]];
					for (size_t b = a + 1; b < by_x.size(); ++b)
					{
						const Creature& o = creatures[order[by_x[b]]];
						if (o.x - c.x >= reach)
							break;
						const double ex = c.x - o.x, ey = c.y - o.y, ez = c.z - o.z;
						const double touch_distance = (c.radius + o.radius) * overlap_fraction;
						if (ex * ex + ey * ey + ez * ez < touch_distance * touch_distance)
						{
							touching[by_x[a]].push_back(by_x[b]);
							touching[by_x[b]].push_back(by_x[a]);
						}
					}
				}

				// Nearest a player first; one touching a creature already kept goes.
				std::vector<size_t> by_player(order.size());
				for (size_t k = 0; k < order.size(); ++k)
					by_player[k] = k;
				std::stable_sort(by_player.begin(), by_player.end(),
					[&](size_t a, size_t b) { return creatures[order[a]].player_distance < creatures[order[b]].player_distance; });
				std::vector<bool> kept(order.size(), false);
				for (size_t k : by_player)
				{
					const Creature* inside = nullptr;
					for (size_t t : touching[k])
					{
						if (kept[t])
						{
							inside = &creatures[order[t]];
							break;
						}
					}
					if (inside == nullptr)
					{
						kept[k] = true;
						continue;
					}
					const Creature& c = creatures[order[k]];
					removed[order[k]] = true;
					plan.remove.push_back(c.id);
					++plan.overlapping;
					if (plan.overlap_samples.size() < kOverlapSamples)
						plan.overlap_samples.push_back({ c.x, c.y, c.z, c.class_index, inside->class_index });
				}
			}
		}

		std::vector<std::vector<const Creature*>> remaining(rules.size());
		for (size_t i = 0; i < creatures.size(); ++i)
		{
			const Creature& creature = creatures[i];
			if (creature.class_index < 0 || static_cast<size_t>(creature.class_index) >= rules.size())
				continue;
			const ClassRule& rule = rules[creature.class_index];
			if (rule.is_protected || removed[i])
				continue;
			if (creature.in_block_area)
			{
				removed[i] = true;
				plan.remove.push_back(creature.id);
				++plan.in_block_area;
				continue;
			}
			++plan.class_counts[creature.class_index];
			for (int g : rule.groups)
			{
				if (valid_group(g))
					++plan.group_counts[g];
			}
			remaining[creature.class_index].push_back(&creature);
		}
		for (auto& list : remaining)
		{
			std::stable_sort(list.begin(), list.end(), [](const Creature* a, const Creature* b)
				{
					return a->player_distance > b->player_distance;
				});
			std::reverse(list.begin(), list.end()); // farthest at the back, so taking is pop_back
		}

		auto uncount = [&](size_t c)
			{
				--plan.class_counts[c];
				for (int g : rules[c].groups)
				{
					if (valid_group(g))
						--plan.group_counts[g];
				}
			};
		auto take = [&](size_t c)
			{
				const Creature* creature = remaining[c].back();
				removed[static_cast<size_t>(creature - creatures.data())] = true;
				plan.remove.push_back(creature->id);
				remaining[c].pop_back();
				uncount(c);
			};

		// 1. Blocked classes: every one.
		for (size_t c = 0; c < rules.size(); ++c)
		{
			while (rules[c].blocked && !remaining[c].empty())
			{
				take(c);
				++plan.blocked;
			}
		}

		// 2. Per-class limits.
		for (size_t c = 0; c < rules.size(); ++c)
		{
			while (rules[c].limit != kNoLimit && plan.class_counts[c] > rules[c].limit && !remaining[c].empty())
			{
				take(c);
				++plan.over_limit;
			}
		}

		// 3. Group limits.
		for (size_t g = 0; g < groups.size(); ++g)
		{
			while (plan.group_counts[g] > groups[g].max)
			{
				size_t best = rules.size();
				for (size_t c = 0; c < rules.size(); ++c)
				{
					const auto& member_of = rules[c].groups;
					if (remaining[c].empty() || std::find(member_of.begin(), member_of.end(), static_cast<int>(g)) == member_of.end())
						continue;
					if (best == rules.size() || remaining[c].size() > remaining[best].size())
						best = c;
				}
				if (best == rules.size())
					break;
				take(best);
				++plan.over_group;
			}
		}

		// 4. Density, outside caves and skip areas.
		std::vector<bool> skips_density(creatures.size(), false);
		if (!density.empty())
		{
			for (size_t i = 0; i < creatures.size(); ++i)
				skips_density[i] = SkipsDensity(skip, creatures[i]);
		}
		plan.hotspots.resize(density.size());
		for (size_t d = 0; d < density.size(); ++d)
		{
			const DensityRule& rule = density[d];
			struct Candidate
			{
				int count = 0;
				double x = 0, y = 0, z = 0;
				std::vector<size_t> list;
			};
			std::vector<Candidate> candidates;
			std::vector<size_t> members;
			for (size_t i = 0; i < creatures.size(); ++i)
			{
				const int c = creatures[i].class_index;
				if (removed[i] || skips_density[i] || c < 0 || static_cast<size_t>(c) >= rules.size() || rules[c].is_protected)
					continue;
				const auto& member_of = rules[c].density;
				if (std::find(member_of.begin(), member_of.end(), static_cast<int>(d)) != member_of.end())
					members.push_back(i);
			}
			if (members.size() <= static_cast<size_t>(rule.max) || rule.size <= 0)
				continue;

			// No more than max in any cube the rule's size across. A second grid shifted by half a cube
			// catches clumps that straddle a cube edge; within a full cube the farthest from players go first.
			auto cell_key = [](double v, double size, double shift)
				{
					return static_cast<int64_t>(std::floor((v + shift) / size));
				};
			auto key3 = [](int64_t x, int64_t y, int64_t z)
				{
					constexpr int64_t offset = int64_t(1) << 20, mask = (int64_t(1) << 21) - 1;
					return (static_cast<uint64_t>((x + offset) & mask) << 42) | (static_cast<uint64_t>((y + offset) & mask) << 21) |
						static_cast<uint64_t>((z + offset) & mask);
				};
			for (const double shift : { 0.0, rule.size / 2 })
			{
				// Cells by sorting (cell, creature) pairs: one run per cell.
				std::vector<std::pair<uint64_t, size_t>> keyed;
				keyed.reserve(members.size());
				for (size_t i : members)
				{
					if (removed[i])
						continue;
					const Creature& c = creatures[i];
					keyed.emplace_back(key3(cell_key(c.x, rule.size, shift), cell_key(c.y, rule.size, shift), cell_key(c.z, rule.size, shift)), i);
				}
				std::sort(keyed.begin(), keyed.end());
				std::vector<std::pair<size_t, size_t>> runs; // (start, size) of every cell with 2 or more
				for (size_t start = 0; start < keyed.size();)
				{
					size_t end = start + 1;
					while (end < keyed.size() && keyed[end].first == keyed[start].first)
						++end;
					if (end - start >= 2)
						runs.emplace_back(start, end - start);
					start = end;
				}
				auto run_list = [&](const std::pair<size_t, size_t>& run)
					{
						std::vector<size_t> list(run.second);
						for (size_t k = 0; k < run.second; ++k)
							list[k] = keyed[run.first + k].second;
						return list;
					};

				// The fullest cells of this pass, before thinning.
				std::vector<size_t> fullest(runs.size());
				for (size_t r = 0; r < runs.size(); ++r)
					fullest[r] = r;
				const size_t keep = (std::min)(fullest.size(), kHotspotsPerRule * 4);
				std::partial_sort(fullest.begin(), fullest.begin() + keep, fullest.end(),
					[&](size_t a, size_t b) { return runs[a].second > runs[b].second; });
				for (size_t f = 0; f < keep; ++f)
				{
					Candidate candidate;
					candidate.list = run_list(runs[fullest[f]]);
					candidate.count = static_cast<int>(candidate.list.size());
					for (size_t i : candidate.list)
					{
						candidate.x += creatures[i].x / candidate.list.size();
						candidate.y += creatures[i].y / candidate.list.size();
						candidate.z += creatures[i].z / candidate.list.size();
					}
					candidates.push_back(std::move(candidate));
				}

				for (const auto& run : runs)
				{
					if (run.second <= static_cast<size_t>(rule.max))
						continue;
					std::vector<size_t> list = run_list(run);
					std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b)
						{
							return creatures[a].player_distance > creatures[b].player_distance;
						});
					for (size_t k = 0; k < list.size() - static_cast<size_t>(rule.max); ++k)
					{
						const Creature& creature = creatures[list[k]];
						removed[list[k]] = true;
						plan.remove.push_back(creature.id);
						uncount(static_cast<size_t>(creature.class_index));
						++plan.too_dense;
					}
				}
			}

			// The fullest cubes, one per spot: the shifted grid sees the same clump again, so a cube whose
			// centre is within half a cube of one already listed is the same spot.
			std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.count > b.count; });
			std::vector<DensityHotspot>& spots = plan.hotspots[d];
			const double same_spot = rule.size * rule.size / 4;
			for (const Candidate& candidate : candidates)
			{
				if (spots.size() >= kHotspotsPerRule)
					break;
				bool seen = false;
				for (const DensityHotspot& spot : spots)
				{
					const double dx = spot.x - candidate.x, dy = spot.y - candidate.y, dz = spot.z - candidate.z;
					seen = seen || dx * dx + dy * dy + dz * dz < same_spot;
				}
				if (seen)
					continue;
				DensityHotspot spot;
				spot.count = candidate.count;
				spot.x = candidate.x;
				spot.y = candidate.y;
				spot.z = candidate.z;
				std::map<int, int> by_class;
				for (size_t i : candidate.list)
					++by_class[creatures[i].class_index];
				for (const auto& [c, n] : by_class)
					spot.classes.emplace_back(n, c);
				std::sort(spot.classes.rbegin(), spot.classes.rend());
				spots.push_back(std::move(spot));
			}
		}
		return plan;
	}

	// For a zone choosing what to spawn: is this class already at everything it may have? Blocked
	// classes always are. counts are the live counts (creatures already queued for removal excluded).
	inline bool IsFull(const ClassRule& rule, int class_count, const std::vector<int>& group_counts,
		const std::vector<GroupRule>& groups)
	{
		if (rule.is_protected)
			return false;
		if (rule.blocked)
			return true;
		if (rule.limit != kNoLimit && class_count >= rule.limit)
			return true;
		for (int g : rule.groups)
		{
			if (g >= 0 && static_cast<size_t>(g) < groups.size() && static_cast<size_t>(g) < group_counts.size() &&
				group_counts[g] >= groups[g].max)
				return true;
		}
		return false;
	}

	// For the spawn hook: should a creature of this class that just appeared be queued for removal,
	// given the counts as they stand after it was added?
	inline bool OverOnSpawn(const ClassRule& rule, int class_count, const std::vector<int>& group_counts,
		const std::vector<GroupRule>& groups)
	{
		if (rule.is_protected)
			return false;
		if (rule.blocked)
			return true;
		if (rule.limit != kNoLimit && class_count > rule.limit)
			return true;
		for (int g : rule.groups)
		{
			if (g >= 0 && static_cast<size_t>(g) < groups.size() && static_cast<size_t>(g) < group_counts.size() &&
				group_counts[g] > groups[g].max)
				return true;
		}
		return false;
	}

	// ------------------------------------------------------------------------------------------
	// Rolling refresh
	// ------------------------------------------------------------------------------------------

	inline bool SkipsRefresh(const Settings& settings, const ClassInfo& info)
	{
		for (const std::string& part : settings.refresh_skip_paths)
		{
			if (ContainsNoCase(info.path, part))
				return true;
		}
		return false;
	}

	struct RefreshCandidate
	{
		size_t id = 0;               // caller's index
		double age_seconds = 0;      // since the creature was first seen
		double player_distance = 0;  // to the nearest player
	};

	// How many creatures to refresh after `elapsed_seconds`, so `wild` creatures turn over in `hours`.
	// The fraction left over is carried to the next call so slow rates still add up.
	inline size_t RefreshQuota(size_t wild, double elapsed_seconds, int hours, double& carry)
	{
		if (hours <= 0 || wild == 0 || elapsed_seconds <= 0)
			return 0;
		carry += static_cast<double>(wild) * elapsed_seconds / (static_cast<double>(hours) * 3600.0);
		const size_t whole = static_cast<size_t>(carry);
		carry -= static_cast<double>(whole);
		return whole;
	}

	// Picks up to `quota` candidates: old enough and far enough from players, oldest first, then the
	// farthest from players.
	inline std::vector<size_t> PlanRefresh(std::vector<RefreshCandidate> candidates, size_t quota,
		double min_age_seconds, double player_distance)
	{
		std::vector<size_t> picked;
		if (quota == 0)
			return picked;
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const RefreshCandidate& c)
			{
				return c.age_seconds < min_age_seconds || c.player_distance < player_distance;
			}), candidates.end());
		std::stable_sort(candidates.begin(), candidates.end(), [](const RefreshCandidate& a, const RefreshCandidate& b)
			{
				if (a.age_seconds != b.age_seconds)
					return a.age_seconds > b.age_seconds;
				return a.player_distance > b.player_distance;
			});
		for (size_t i = 0; i < candidates.size() && picked.size() < quota; ++i)
			picked.push_back(candidates[i].id);
		return picked;
	}

	// A comma right before ] or } (whitespace and comments between allowed) is removed before parsing, so a
	// list edited down to its last entry still loads. Strings and comments are left alone. Returns the line
	// of every comma removed.
	inline std::vector<int> StripTrailingCommas(std::string& text)
	{
		std::vector<int> lines;
		std::string out;
		out.reserve(text.size());
		int line = 1;
		auto next_significant = [&](size_t from)
			{
				size_t i = from;
				while (i < text.size())
				{
					const char c = text[i];
					if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
						++i;
					else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
					{
						while (i < text.size() && text[i] != '\n')
							++i;
					}
					else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
					{
						const size_t end = text.find("*/", i + 2);
						i = end == std::string::npos ? text.size() : end + 2;
					}
					else
						return c;
				}
				return '\0';
			};
		for (size_t i = 0; i < text.size(); ++i)
		{
			const char c = text[i];
			if (c == '\n')
				++line;
			if (c == '"')
			{
				// Copy the whole string, escapes included.
				out += c;
				for (++i; i < text.size(); ++i)
				{
					out += text[i];
					if (text[i] == '\\' && i + 1 < text.size())
						out += text[++i];
					else if (text[i] == '"')
						break;
					else if (text[i] == '\n')
						++line;
				}
				continue;
			}
			if (c == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*'))
			{
				const bool block = text[i + 1] == '*';
				const size_t end = block ? text.find("*/", i + 2) : text.find('\n', i);
				const size_t stop = end == std::string::npos ? text.size() : (block ? end + 2 : end);
				for (; i < stop; ++i)
				{
					out += text[i];
					if (text[i] == '\n')
						++line;
				}
				--i;
				continue;
			}
			if (c == ',')
			{
				const char after = next_significant(i + 1);
				if (after == ']' || after == '}')
				{
					lines.push_back(line);
					continue;
				}
			}
			out += c;
		}
		text.swap(out);
		return lines;
	}

	// The whole config.json text to settings: trailing commas tolerated (each one a warning), comments allowed.
	inline Settings ParseConfigText(std::string text)
	{
		const std::vector<int> commas = StripTrailingCommas(text);
		nlohmann::json root;
		try
		{
			root = nlohmann::json::parse(text, nullptr, true, true);
		}
		catch (const nlohmann::json::exception& error)
		{
			throw std::runtime_error(std::string("config.json is not valid JSON: ") + error.what());
		}
		Settings settings = ParseSettings(root);
		std::vector<std::string> comma_warnings;
		for (int line : commas)
			comma_warnings.push_back("line " + std::to_string(line) + ": comma before a closing ] or } ignored; remove it to keep the file valid JSON");
		settings.warnings.insert(settings.warnings.begin(), comma_warnings.begin(), comma_warnings.end());
		return settings;
	}
}
