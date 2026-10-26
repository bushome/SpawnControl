#pragma once

// Pure logic for SpawnControl: config parsing, class rules (block, limit, group limit, protection),
// and the plan of which wild creatures to remove.
//
// No dependency on the game or on server API types (only on the nlohmann json.hpp that ships with
// AsaApi and AseApi), so it compiles and unit-tests on any machine. Identical in the ASA and ASE
// folders; keep the two copies in sync. See tests/logic_tests.cpp.

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
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

	struct MapRules
	{
		std::string name;                               // section name as written in config.json
		std::vector<std::string> block;
		std::vector<std::pair<std::string, int>> limits; // class key -> max wild count
		std::vector<GroupRule> groups;
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
			return rules;
		}
	}

	// Throws only for a document that is not an object; everything else becomes a warning and a default.
	inline Settings ParseSettings(const nlohmann::json& root)
	{
		if (!root.is_object())
			throw std::runtime_error("config.json must be a JSON object");

		Settings settings;
		std::vector<std::string>& warnings = settings.warnings;

		const nlohmann::json general = root.value("General", nlohmann::json::object());
		settings.removals_per_tick = detail::ReadInt(general, "RemovalsPerTick", settings.removals_per_tick, 1, warnings);
		settings.census_actors_per_tick = detail::ReadInt(general, "CensusActorsPerTick", settings.census_actors_per_tick, 100, warnings);
		settings.census_interval_seconds = detail::ReadInt(general, "CensusIntervalSeconds", settings.census_interval_seconds, 5, warnings);
		settings.near_player_distance = detail::ReadInt(general, "NearPlayerDistance", static_cast<int>(settings.near_player_distance), 0, warnings);
		for (const auto& [key, target] : { std::pair<const char*, bool*>{ "LogRemovals", &settings.log_removals },
				std::pair<const char*, bool*>{ "RefuseFullEntries", &settings.refuse_full_entries } })
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
		return rule;
	}

	inline bool HasAnyRule(const ClassRule& rule)
	{
		return rule.blocked || rule.limit != kNoLimit || !rule.groups.empty();
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
	};

	struct Plan
	{
		std::vector<size_t> remove;               // ids, in the order to remove them
		std::vector<int> class_counts;            // wild count per class index
		std::vector<int> group_counts;            // wild count per group
		size_t blocked = 0;
		size_t over_limit = 0;
		size_t over_group = 0;
	};

	// Which creatures to remove so every block, limit and group limit holds. Blocked creatures come
	// first; within each rule the creatures farthest from any player go first, so removals happen where
	// nobody sees them. A creature is only ever picked once. Group limits take from whichever member
	// class has the most left, so 250 vanilla + 250 modded otters capped at 200 end near 100 + 100.
	inline Plan PlanRemovals(const std::vector<ClassRule>& rules, const std::vector<GroupRule>& groups,
		const std::vector<Creature>& creatures)
	{
		Plan plan;
		plan.class_counts.assign(rules.size(), 0);
		plan.group_counts.assign(groups.size(), 0);

		auto valid_group = [&](int g) { return g >= 0 && static_cast<size_t>(g) < groups.size(); };

		// Per class: what is still unpicked, farthest from players first (ties keep census order).
		std::vector<std::vector<const Creature*>> remaining(rules.size());
		for (const Creature& creature : creatures)
		{
			if (creature.class_index < 0 || static_cast<size_t>(creature.class_index) >= rules.size())
				continue;
			const ClassRule& rule = rules[creature.class_index];
			if (rule.is_protected)
				continue;
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

		auto take = [&](size_t c)
			{
				plan.remove.push_back(remaining[c].back()->id);
				remaining[c].pop_back();
				--plan.class_counts[c];
				for (int g : rules[c].groups)
				{
					if (valid_group(g))
						--plan.group_counts[g];
				}
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
}
