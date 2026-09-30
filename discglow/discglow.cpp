/*
 * DiscGlow: a small standalone DLL for Echo VR (final PC build, goldmaster 631547) that gives the personal disc's grid
 * sphere its 2018 orange again, lets the team colours of the disc sphere be changed, and keeps a disc's team colour
 * until the other team touches it.
 *
 * How it works: the game colours model instances (e.g. the disc sphere per team) through one engine function
 * (SetInstanceModelColor, at +0xcc5900). DiscGlow hooks it:
 *   - team colours the game sets on a disc are swapped for the chosen ones (fades keep their brightness);
 *   - the fade back to neutral after a touch is replaced by the last team colour (StickyTeamColour);
 *   - the personal disc's sphere is never coloured by the game, so it keeps the default white: a few times a second
 *     DiscGlow paints plain-white instances that move in the model components that hold discs.
 * Components holding discs are recognised by the neutral colour the game gives discs, and by the lobby's fixed type.
 * DiscGlow.ini is re-read while the game runs, so changed colours apply immediately.
 *
 * Loaded as dinput8.dll next to echovr.exe. DirectInput8Create is forwarded to dinput8.chain.dll when present (another
 * mod that used the dinput8.dll name, e.g. ReShade, which is loaded as well) or else to the system dinput8.dll.
 * If the game version differs (the hooked function's bytes do not match), DiscGlow does nothing.
 *
 * DiscGlow.ini (next to the DLL), section [DiscGlow]:
 *   PersonalDisc=1                  paint personal disc spheres
 *   PersonalDiscColour=1 0.5 0.15   linear RGB, values above 1 glow brighter
 *   BlueTeamColour=0 0.698 1        colour for the blue team's disc (the game's own colour by default)
 *   OrangeTeamColour=1 0.5 0.15     colour for the orange team's disc (the game's own colour by default)
 *   PersonalDiscSaturation=1        also BlueTeamSaturation, OrangeTeamSaturation: 0 grey, 1 unchanged, 2 extra vivid
 *   PersonalDiscEffect=none         also BlueTeamEffect, OrangeTeamEffect: none, rainbow, strobe or pulse
 *   PersonalDiscEffectSpeed=2       seconds per effect cycle (also BlueTeamEffectSpeed, OrangeTeamEffectSpeed)
 *   ChassisGlow=1                   0 turns the glow of player chassis off (their customisation tint colours become black)
 *   StickyTeamColour=1              keep a disc's team colour until the other team touches it (no fade back to neutral)
 *   Replace1=r g b > r g b          up to Replace8: swap any other colour the game sets
 *   Log=0                           write DiscGlow.log
 *   Debug=0                         with Log=1: log every distinct colour call, F9 writes a snapshot of all instances
 */

#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <share.h>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include "include/MinHook.h"

namespace
{
	// FUN_140cc5900 in the goldmaster build: void SetInstanceModelColor(model component, instance, const float rgba[4], int)
	constexpr uintptr_t SET_COLOR_RVA = 0xcc5900;
	constexpr uint8_t SET_COLOR_BYTES[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30 };
	// FUN_140bbe870: void SetLightColor(light component, index, const float rgb[3]); the disc's light on the surroundings
	// is coloured through it by R15FrisbeeSetLightColorNode
	constexpr uintptr_t SET_LIGHT_RVA = 0xbbe870;
	constexpr uint8_t SET_LIGHT_BYTES[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20 };

	// Model component layout (from the function that applies a colour, FUN_1404c57b0):
	//   *(component + 0x80)                  owning system; the component sits at a fixed offset inside it per component type
	//   *(*(component + 0x8d0) + 8)          u16 slot per instance, 4 bytes apart
	//   *(component + 0x8e0) + slot * 0x48   instance record: rotation (4 floats), position (3), scale (3), 2 more, colour RGBA
	constexpr uintptr_t PERSONAL_DISC_GROUP = 0x624d8; // Offset of the lobby's component type that holds the personal discs
	constexpr uint32_t MAX_INSTANCES = 4096;

	// Colours the game gives a disc sphere
	constexpr float DISC_NEUTRAL[3] = { 0.4745f, 0.4275f, 0.4588f };
	constexpr float TEAM_COLOURS[2][3] = { { 0.0f, 0.698f, 1.0f }, { 1.0f, 0.5f, 0.15f } }; // Blue, orange

	using set_color_fn = void(__fastcall *)(uintptr_t component, uint64_t instance, float *rgba, uint32_t extra);
	set_color_fn s_original = nullptr;
	using set_light_fn = void(__fastcall *)(uintptr_t component, uint64_t index, float *rgb);
	set_light_fn s_original_light = nullptr;
	// FUN_14091eb60: applies a customisation tint to a model instance: (component, instance, override on, colour A, colour B).
	// Called by R15NetCustomization::OverrideTint with the equipped tint item's two colours; they colour the chassis glow.
	constexpr uintptr_t SET_TINT_RVA = 0x91eb60;
	constexpr uint8_t SET_TINT_BYTES[] = { 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x41, 0xc1, 0xe0, 0x05, 0x44 };
	using set_tint_fn = void(__fastcall *)(uintptr_t component, uint64_t instance, int enable, float *a, float *b);
	set_tint_fn s_original_tint = nullptr;

	uintptr_t s_base = 0;
	wchar_t s_dir[MAX_PATH] = L"";
	HMODULE s_chain = nullptr;
	FILE *s_file = nullptr;

	// Everything below is guarded by s_mutex
	std::mutex s_mutex;

	// Settings
	bool s_log = false, s_debug = false, s_personal_disc = true, s_sticky = true, s_chassis_glow = true;
	float s_personal_colour[4] = { 1.0f, 0.5f, 0.15f, 1.0f };
	struct replace_rule
	{
		float from[3], to[3];
	};
	std::vector<replace_rule> s_rules;
	// Colour effects per disc: 0 personal, 1 blue team, 2 orange team
	enum effect_mode { EFFECT_NONE, EFFECT_RAINBOW, EFFECT_STROBE, EFFECT_PULSE };
	struct effect
	{
		int mode = EFFECT_NONE;
		float period = 2.0f; // Seconds per cycle
	};
	effect s_effects[3];
	float s_team_colours[2][3] = {}; // The chosen team colours (after saturation), the base for effects

	// Components the game has coloured: component -> (owning system, vtable) when first seen
	std::map<uintptr_t, std::pair<uintptr_t, uintptr_t>> s_components;
	// Components that hold discs (the game gave something in them the disc neutral colour)
	std::set<uintptr_t> s_disc_components;
	// Personal disc spheres DiscGlow painted, to repaint them when the colour setting changes
	std::set<std::pair<uintptr_t, uint64_t>> s_painted;
	// Last team colour per disc instance, as the game sent it
	struct sticky_state
	{
		int team = -1;              // 0 blue, 1 orange, -1 none
		float raw[4] = {};          // The game's colour for that team (before swapping)
		bool last_was_team = false; // The previous call was a team colour (not part of a fade)
	};
	std::map<std::pair<uintptr_t, uint64_t>, sticky_state> s_sticky_states;
	std::set<std::tuple<uintptr_t, uintptr_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>> s_seen;
	std::set<std::tuple<uintptr_t, uint64_t, uint32_t, uint32_t, uint32_t>> s_seen_lights;
	std::set<std::tuple<uintptr_t, uint64_t, int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> s_seen_tints;

	void log_line(const char *format, ...)
	{
		if (s_file == nullptr)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::fprintf(s_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, format);
		std::vfprintf(s_file, format, args);
		va_end(args);
		std::fputc('\n', s_file);
		std::fflush(s_file);
	}

	uint32_t bits(float value)
	{
		uint32_t result;
		std::memcpy(&result, &value, sizeof(result));
		return result;
	}

	// Copies memory that may be invalid; returns false instead of crashing
	bool safe_copy(void *dest, const void *source, size_t size)
	{
		__try
		{
			std::memcpy(dest, source, size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	template <typename T>
	bool safe_read(uintptr_t address, T &value)
	{
		return address != 0 && safe_copy(&value, reinterpret_cast<const void *>(address), sizeof(T));
	}

	// Brightness factor of 'rgba' as a multiple of 'base', or 0 when it is not that colour (at 20 % brightness or more)
	float match_scale(const float *rgba, const float *base)
	{
		const float base_sum = base[0] + base[1] + base[2];
		const float scale = base_sum > 0 ? (rgba[0] + rgba[1] + rgba[2]) / base_sum : 0;
		if (scale < 0.2f) // Nearly faded out colours are too dim to tell apart
			return 0;
		for (int c = 0; c < 3; ++c)
			if (std::fabs(rgba[c] - base[c] * scale) >= 0.01f * scale)
				return 0;
		return scale;
	}

	int team_of(const float *rgba)
	{
		for (int team = 0; team < 2; ++team)
			if (match_scale(rgba, TEAM_COLOURS[team]) > 0)
				return team;
		return -1;
	}

	bool is_neutral(const float *rgba)
	{
		return std::fabs(rgba[0] - DISC_NEUTRAL[0]) < 0.005f && std::fabs(rgba[1] - DISC_NEUTRAL[1]) < 0.005f && std::fabs(rgba[2] - DISC_NEUTRAL[2]) < 0.005f;
	}

	// Called with s_mutex held: swaps the colour when it matches a rule, keeping the brightness of fades
	void apply_rules(float *rgba)
	{
		for (const replace_rule &rule : s_rules)
			if (const float scale = match_scale(rgba, rule.from))
			{
				for (int c = 0; c < 3; ++c)
					rgba[c] = rule.to[c] * scale;
				return;
			}
	}

	struct component_view
	{
		uintptr_t slots = 0, records = 0;

		bool open(uintptr_t component)
		{
			uintptr_t mapping = 0;
			return safe_read(component + 0x8d0, mapping) && safe_read(mapping + 8, slots) && safe_read(component + 0x8e0, records) && slots != 0 && records != 0;
		}
		// Returns false at the end of the instance table
		bool read(uint32_t instance, uint16_t &slot, float (&record)[16], bool &valid) const
		{
			uint16_t entry[2];
			if (!safe_read(slots + instance * 4ull, entry))
				return false;
			slot = entry[0];
			valid = slot != 0xffff && safe_copy(record, reinterpret_cast<const void *>(records + slot * 0x48ull), sizeof(record));
			return true;
		}
	};

	// The component is still the one first seen (not freed and reused by another level)
	bool still_valid(uintptr_t component, uintptr_t system, uintptr_t vtable)
	{
		uintptr_t current_system = 0, current_vtable = 0;
		return safe_read(component + 0x80, current_system) && safe_read(component, current_vtable) && current_system == system && current_vtable == vtable;
	}

	bool still_valid(uintptr_t component)
	{
		const auto it = s_components.find(component);
		return it != s_components.end() && still_valid(component, it->second.first, it->second.second);
	}

	// ---- Settings ----

	std::wstring ini_path()
	{
		return std::wstring(s_dir) + L"\\DiscGlow.ini";
	}

	bool read_rgb(const wchar_t *key, float (&rgb)[3])
	{
		wchar_t value[128];
		GetPrivateProfileStringW(L"DiscGlow", key, L"", value, 128, ini_path().c_str());
		return swscanf_s(value, L"%f %f %f", &rgb[0], &rgb[1], &rgb[2]) == 3;
	}

	// Scales how far the colour is from grey of the same brightness (0 grey, 1 unchanged, 2 twice as vivid)
	void saturate(float (&rgb)[3], const wchar_t *key)
	{
		wchar_t value[64];
		GetPrivateProfileStringW(L"DiscGlow", key, L"1", value, 64, ini_path().c_str());
		float saturation = 1;
		if (swscanf_s(value, L"%f", &saturation) != 1 || saturation == 1)
			return;
		const float luminance = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
		for (float &c : rgb)
			c = (std::max)(0.0f, luminance + (c - luminance) * saturation);
	}

	// Called with s_mutex held
	void load_config()
	{
		const std::wstring path = ini_path();
		s_log = GetPrivateProfileIntW(L"DiscGlow", L"Log", 0, path.c_str()) != 0;
		s_debug = GetPrivateProfileIntW(L"DiscGlow", L"Debug", 0, path.c_str()) != 0;
		s_personal_disc = GetPrivateProfileIntW(L"DiscGlow", L"PersonalDisc", 1, path.c_str()) != 0;
		s_sticky = GetPrivateProfileIntW(L"DiscGlow", L"StickyTeamColour", 1, path.c_str()) != 0;
		s_chassis_glow = GetPrivateProfileIntW(L"DiscGlow", L"ChassisGlow", 1, path.c_str()) != 0;
		float rgb[3];
		if (read_rgb(L"PersonalDiscColour", rgb))
		{
			saturate(rgb, L"PersonalDiscSaturation");
			std::memcpy(s_personal_colour, rgb, sizeof(rgb));
		}

		s_rules.clear();
		const wchar_t *team_keys[2] = { L"BlueTeamColour", L"OrangeTeamColour" };
		const wchar_t *saturation_keys[2] = { L"BlueTeamSaturation", L"OrangeTeamSaturation" };
		for (int team = 0; team < 2; ++team)
		{
			bool set = read_rgb(team_keys[team], rgb);
			if (!set)
				std::memcpy(rgb, TEAM_COLOURS[team], sizeof(rgb)); // Saturation alone also applies to the game's own colour
			float before[3];
			std::memcpy(before, rgb, sizeof(before));
			saturate(rgb, saturation_keys[team]);
			std::memcpy(s_team_colours[team], rgb, sizeof(rgb));
			if (set || std::memcmp(before, rgb, sizeof(rgb)) != 0)
			{
				replace_rule rule;
				std::memcpy(rule.from, TEAM_COLOURS[team], sizeof(rule.from));
				std::memcpy(rule.to, rgb, sizeof(rule.to));
				s_rules.push_back(rule);
			}
		}
		for (int i = 1; i <= 8; ++i)
		{
			wchar_t key[16], value[128];
			swprintf_s(key, L"Replace%d", i);
			GetPrivateProfileStringW(L"DiscGlow", key, L"", value, 128, path.c_str());
			replace_rule rule;
			if (swscanf_s(value, L"%f %f %f > %f %f %f", &rule.from[0], &rule.from[1], &rule.from[2], &rule.to[0], &rule.to[1], &rule.to[2]) == 6)
				s_rules.push_back(rule);
		}

		// Effects: <Name>Effect=none|rainbow|strobe|pulse, <Name>EffectSpeed=seconds per cycle
		const wchar_t *effect_names[3] = { L"PersonalDisc", L"BlueTeam", L"OrangeTeam" };
		for (int i = 0; i < 3; ++i)
		{
			wchar_t key[64], value[64];
			swprintf_s(key, L"%sEffect", effect_names[i]);
			GetPrivateProfileStringW(L"DiscGlow", key, L"none", value, 64, path.c_str());
			s_effects[i].mode = _wcsicmp(value, L"rainbow") == 0 ? EFFECT_RAINBOW : _wcsicmp(value, L"strobe") == 0 ? EFFECT_STROBE :
				_wcsicmp(value, L"pulse") == 0 ? EFFECT_PULSE : EFFECT_NONE;
			swprintf_s(key, L"%sEffectSpeed", effect_names[i]);
			GetPrivateProfileStringW(L"DiscGlow", key, L"2", value, 64, path.c_str());
			float period = 2;
			s_effects[i].period = swscanf_s(value, L"%f", &period) == 1 ? (std::max)(0.05f, period) : 2.0f;
		}
	}

	// The colour an effect gives at time 't' (seconds), based on 'base' (keeps its brightness)
	void animate(const float *base, const effect &e, double t, float (&out)[4])
	{
		const double phase = std::fmod(t / e.period, 1.0);
		const float peak = (std::max)(base[0], (std::max)(base[1], base[2]));
		switch (e.mode)
		{
		case EFFECT_RAINBOW:
		{
			// Full-saturation hue wheel at the base colour's brightness
			const float h = static_cast<float>(phase) * 6.0f, x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
			const float wheel[6][3] = { { 1, x, 0 }, { x, 1, 0 }, { 0, 1, x }, { 0, x, 1 }, { x, 0, 1 }, { 1, 0, x } };
			const float *c = wheel[(std::min)(5, static_cast<int>(h))];
			const float value = (std::max)(0.5f, peak);
			for (int i = 0; i < 3; ++i)
				out[i] = c[i] * value;
			break;
		}
		case EFFECT_STROBE:
			for (int i = 0; i < 3; ++i)
				out[i] = phase < 0.5 ? base[i] : base[i] * 0.03f;
			break;
		case EFFECT_PULSE:
		{
			const float k = 0.3f + 0.7f * (0.5f + 0.5f * static_cast<float>(std::cos(phase * 6.283185307)));
			for (int i = 0; i < 3; ++i)
				out[i] = base[i] * k;
			break;
		}
		default:
			std::memcpy(out, base, 3 * sizeof(float));
			break;
		}
		out[3] = 1.0f;
	}

	double now_seconds()
	{
		return GetTickCount64() / 1000.0;
	}

	FILETIME ini_time()
	{
		WIN32_FILE_ATTRIBUTE_DATA data = {};
		GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &data);
		return data.ftLastWriteTime;
	}

	// Re-applies colours after the settings changed: repaints personal discs and discs holding a team colour
	void reapply_colours()
	{
		std::vector<std::pair<std::pair<uintptr_t, uint64_t>, std::vector<float>>> updates;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			for (auto it = s_painted.begin(); it != s_painted.end();)
			{
				if (!still_valid(it->first))
				{
					it = s_painted.erase(it);
					continue;
				}
				if (s_personal_disc)
					updates.push_back({ *it, std::vector<float>(s_personal_colour, s_personal_colour + 4) });
				++it;
			}
			for (auto it = s_sticky_states.begin(); it != s_sticky_states.end();)
			{
				if (!still_valid(it->first.first))
				{
					it = s_sticky_states.erase(it);
					continue;
				}
				if (it->second.team >= 0)
				{
					float colour[4];
					std::memcpy(colour, it->second.raw, sizeof(colour));
					apply_rules(colour);
					updates.push_back({ it->first, std::vector<float>(colour, colour + 4) });
				}
				++it;
			}
		}
		for (auto &update : updates)
			s_original(update.first.first, update.first.second, update.second.data(), 0);
	}

	// ---- Effects ----

	// Animates personal disc spheres and discs holding a team colour that have an effect, about 30 times a second
	DWORD WINAPI effect_thread(void *)
	{
		std::vector<std::pair<std::pair<uintptr_t, uint64_t>, std::vector<float>>> updates;
		for (;;)
		{
			Sleep(33);
			updates.clear();
			{
				const std::lock_guard<std::mutex> lock(s_mutex);
				const double t = now_seconds();
				float colour[4];
				if (s_personal_disc && s_effects[0].mode != EFFECT_NONE)
					for (const auto &painted : s_painted)
						if (still_valid(painted.first))
						{
							animate(s_personal_colour, s_effects[0], t, colour);
							updates.push_back({ painted, std::vector<float>(colour, colour + 4) });
						}
				for (const auto &entry : s_sticky_states)
				{
					const int team = entry.second.team;
					if (team >= 0 && s_effects[1 + team].mode != EFFECT_NONE && still_valid(entry.first.first))
					{
						animate(s_team_colours[team], s_effects[1 + team], t, colour);
						updates.push_back({ entry.first, std::vector<float>(colour, colour + 4) });
					}
				}
			}
			for (auto &update : updates)
				s_original(update.first.first, update.first.second, update.second.data(), 0);
		}
	}

	// ---- Personal discs ----

	// Paints plain-white instances that move in the components holding discs; also reloads changed settings
	DWORD WINAPI personal_disc_thread(void *)
	{
		std::map<std::pair<uintptr_t, uint16_t>, std::tuple<float, float, float>> last_positions;
		FILETIME last_ini_time = ini_time();
		for (unsigned int tick = 0;; ++tick)
		{
			Sleep(250);

			if (tick % 2 == 0)
			{
				const FILETIME time = ini_time();
				if (CompareFileTime(&time, &last_ini_time) != 0)
				{
					last_ini_time = time;
					Sleep(50); // Let the writer finish
					{
						const std::lock_guard<std::mutex> lock(s_mutex);
						load_config();
						log_line("Settings reloaded.");
					}
					reapply_colours();
				}
			}

			std::vector<std::tuple<uintptr_t, uintptr_t, uintptr_t>> targets;
			float colour[4];
			{
				const std::lock_guard<std::mutex> lock(s_mutex);
				if (!s_personal_disc)
					continue;
				std::memcpy(colour, s_personal_colour, sizeof(colour));
				for (const auto &entry : s_components)
					if ((s_disc_components.count(entry.first) != 0 || entry.first - entry.second.first == PERSONAL_DISC_GROUP) &&
						still_valid(entry.first, entry.second.first, entry.second.second))
						targets.emplace_back(entry.first, entry.second.first, entry.second.second);
			}

			std::map<std::pair<uintptr_t, uint16_t>, std::tuple<float, float, float>> positions;
			for (const auto &target : targets)
			{
				const uintptr_t component = std::get<0>(target), system = std::get<1>(target), vtable = std::get<2>(target);
				component_view view;
				if (!view.open(component))
					continue;
				std::set<uint16_t> done;
				uint16_t slot;
				float record[16];
				bool valid;
				for (uint32_t instance = 0; instance < MAX_INSTANCES && view.read(instance, slot, record, valid); ++instance)
				{
					if (!valid || !done.insert(slot).second)
						continue; // Several instances can share one record
					const auto key = std::make_pair(component, slot);
					positions[key] = std::make_tuple(record[4], record[5], record[6]);
					if (record[12] != 1.0f || record[13] != 1.0f || record[14] != 1.0f || (record[4] == 0 && record[5] == 0 && record[6] == 0))
						continue; // Coloured by the game or by us already, or not placed in the world
					const auto last = last_positions.find(key);
					if (last == last_positions.end())
						continue;
					const float dx = record[4] - std::get<0>(last->second), dy = record[5] - std::get<1>(last->second), dz = record[6] - std::get<2>(last->second);
					if (dx * dx + dy * dy + dz * dz < 0.05f * 0.05f)
						continue; // Only discs move; static white models of these types are left alone
					if (!still_valid(component, system, vtable))
						break;
					float paint[4];
					std::memcpy(paint, colour, sizeof(paint));
					s_original(component, instance, paint, 0);
					const std::lock_guard<std::mutex> lock(s_mutex);
					s_painted.emplace(component, instance);
					if (s_log)
						log_line("personal disc: painted component %p slot %u (instance %u) at %.2f %.2f %.2f", reinterpret_cast<void *>(component), slot, instance, record[4], record[5], record[6]);
				}
			}
			last_positions = std::move(positions);
		}
	}

	// ---- Debug ----

	// Writes every instance record of every known model component to DiscGlow.snapshot<N>.txt
	void snapshot()
	{
		static int s_index = 0;
		std::map<uintptr_t, std::pair<uintptr_t, uintptr_t>> components;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			components = s_components;
		}
		wchar_t path[MAX_PATH];
		swprintf_s(path, L"%s\\DiscGlow.snapshot%d.txt", s_dir, ++s_index);
		FILE *file = nullptr;
		if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
			return;
		for (const auto &entry : components)
		{
			const uintptr_t component = entry.first;
			component_view view;
			std::fprintf(file, "component %p (offset %#llx in its system)\n", reinterpret_cast<void *>(component), static_cast<unsigned long long>(component - entry.second.first));
			if (!still_valid(component, entry.second.first, entry.second.second) || !view.open(component))
				continue;
			uint16_t slot;
			float record[16];
			bool valid;
			for (uint32_t instance = 0; instance < MAX_INSTANCES && view.read(instance, slot, record, valid); ++instance)
				if (valid)
					std::fprintf(file, "  instance %u slot %u pos %.3f %.3f %.3f scale %.2f %.2f %.2f colour %.3f %.3f %.3f %.3f\n",
						instance, slot, record[4], record[5], record[6], record[7], record[8], record[9], record[12], record[13], record[14], record[15]);
		}
		std::fclose(file);
		const std::lock_guard<std::mutex> lock(s_mutex);
		log_line("wrote snapshot %d (%zu components)", s_index, components.size());
	}

	DWORD WINAPI key_thread(void *)
	{
		bool was_down = false;
		for (;;)
		{
			Sleep(50);
			const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			if (down && !was_down)
				snapshot();
			was_down = down;
		}
	}

	// ---- Hook ----

	void __fastcall hooked_set_color(uintptr_t component, uint64_t instance, float *rgba, uint32_t extra)
	{
		float out[4];
		bool changed = false;
		if (rgba != nullptr)
		{
			const std::lock_guard<std::mutex> lock(s_mutex);

			uintptr_t system = 0, vtable = 0;
			const auto it = s_components.find(component);
			if (safe_read(component + 0x80, system) && safe_read(component, vtable) && (it == s_components.end() || it->second != std::make_pair(system, vtable)))
				s_components[component] = { system, vtable };
			if (is_neutral(rgba) && s_disc_components.insert(component).second && s_log)
				log_line("disc component found: %p (offset %#llx in its system)", reinterpret_cast<void *>(component), static_cast<unsigned long long>(component - system));

			std::memcpy(out, rgba, sizeof(out));
			bool keep = false, animated = false;
			if (s_disc_components.count(component) != 0)
			{
				sticky_state &state = s_sticky_states[{ component, instance }];
				const int team = team_of(rgba);
				if (team >= 0)
				{
					// A team colour (including the grab pulse): remember it
					state.team = team;
					std::memcpy(state.raw, rgba, sizeof(state.raw));
					state.last_was_team = true;
				}
				else if (s_sticky && state.team >= 0 && !(is_neutral(rgba) && state.last_was_team))
				{
					// Part of the fade back to neutral (or its end): keep the team colour instead.
					// A direct jump from the team colour to neutral (new round) is let through.
					std::memcpy(out, state.raw, sizeof(out));
					state.last_was_team = false;
					keep = true;
				}
				else
				{
					state.team = -1;
					state.last_was_team = false;
				}
				// A team with an effect: the effect thread owns the colour; send its current value so the game's call does not flicker
				if (state.team >= 0 && s_effects[1 + state.team].mode != EFFECT_NONE)
				{
					animate(s_team_colours[state.team], s_effects[1 + state.team], now_seconds(), out);
					animated = keep = true;
				}
			}
			if (!animated)
				apply_rules(out);
			changed = keep || std::memcmp(out, rgba, sizeof(out)) != 0;

			if (s_log && s_debug)
			{
				const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - s_base;
				if (s_seen.emplace(caller, component, instance, bits(rgba[0]), bits(rgba[1]), bits(rgba[2]), bits(rgba[3])).second)
					log_line("caller +%llx component %p (group %#llx) instance %llu extra %u colour %.4f %.4f %.4f %.4f%s",
						static_cast<unsigned long long>(caller), reinterpret_cast<void *>(component), static_cast<unsigned long long>(component - system), static_cast<unsigned long long>(instance), extra,
						rgba[0], rgba[1], rgba[2], rgba[3], changed ? " (changed)" : "");
			}
		}
		s_original(component, instance, changed ? out : rgba, extra);
	}

	// The disc's light on its surroundings: team colours are swapped like the sphere's (same rules)
	void __fastcall hooked_set_light(uintptr_t component, uint64_t index, float *rgb)
	{
		float out[4] = {};
		bool changed = false;
		if (rgb != nullptr)
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			if (s_log && s_seen_lights.emplace(component, index, bits(rgb[0]), bits(rgb[1]), bits(rgb[2])).second)
				log_line("light: component %p index %llu colour %.4f %.4f %.4f", reinterpret_cast<void *>(component), static_cast<unsigned long long>(index), rgb[0], rgb[1], rgb[2]);
			std::memcpy(out, rgb, 3 * sizeof(float));
			apply_rules(out);
			changed = std::memcmp(out, rgb, 3 * sizeof(float)) != 0;
		}
		s_original_light(component, index, changed ? out : rgb);
	}

	// Chassis customisation tint: with ChassisGlow=0 both tint colours become black (override forced on)
	void __fastcall hooked_set_tint(uintptr_t component, uint64_t instance, int enable, float *a, float *b)
	{
		bool glow;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			glow = s_chassis_glow;
			if (s_log && a != nullptr && b != nullptr &&
				s_seen_tints.emplace(component, instance & 0xffff, enable, bits(a[0]), bits(a[1]), bits(a[2]), bits(b[0]), bits(b[1]), bits(b[2])).second)
			{
				uintptr_t system = 0;
				safe_read(component + 0x80, system);
				log_line("tint: component %p (group %#llx) instance %llu override %d A %.4f %.4f %.4f B %.4f %.4f %.4f", reinterpret_cast<void *>(component),
					static_cast<unsigned long long>(component - system), static_cast<unsigned long long>(instance & 0xffff), enable, a[0], a[1], a[2], b[0], b[1], b[2]);
			}
		}
		if (!glow)
		{
			float black_a[3] = {}, black_b[3] = {};
			s_original_tint(component, instance, 1, black_a, black_b);
			return;
		}
		s_original_tint(component, instance, enable, a, b);
	}

	DWORD WINAPI install(void *)
	{
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			load_config();
			if (s_log)
				s_file = _wfsopen((std::wstring(s_dir) + L"\\DiscGlow.log").c_str(), L"w", _SH_DENYWR); // Others can read the log while the game runs
		}

		s_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		void *const target = reinterpret_cast<void *>(s_base + SET_COLOR_RVA);
		if (std::memcmp(target, SET_COLOR_BYTES, sizeof(SET_COLOR_BYTES)) != 0)
		{
			log_line("Game version not recognised (colour function bytes differ), nothing hooked.");
			return 0;
		}
		if (MH_Initialize() != MH_OK ||
			MH_CreateHook(target, reinterpret_cast<void *>(&hooked_set_color), reinterpret_cast<void **>(&s_original)) != MH_OK ||
			MH_EnableHook(target) != MH_OK)
		{
			log_line("Could not hook the colour function.");
			return 0;
		}
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			log_line("DiscGlow active: personal disc %s, %zu colour swap(s), sticky %s, chained dinput8 %s.", s_personal_disc ? "on" : "off",
				s_rules.size(), s_sticky ? "on" : "off", s_chain != nullptr ? "loaded" : "none");
		}
		// The disc light hook is optional: without it only the light keeps the game's colours
		void *const light_target = reinterpret_cast<void *>(s_base + SET_LIGHT_RVA);
		const bool light_hooked = std::memcmp(light_target, SET_LIGHT_BYTES, sizeof(SET_LIGHT_BYTES)) == 0 &&
			MH_CreateHook(light_target, reinterpret_cast<void *>(&hooked_set_light), reinterpret_cast<void **>(&s_original_light)) == MH_OK &&
			MH_EnableHook(light_target) == MH_OK;
		void *const tint_target = reinterpret_cast<void *>(s_base + SET_TINT_RVA);
		const bool tint_hooked = std::memcmp(tint_target, SET_TINT_BYTES, sizeof(SET_TINT_BYTES)) == 0 &&
			MH_CreateHook(tint_target, reinterpret_cast<void *>(&hooked_set_tint), reinterpret_cast<void **>(&s_original_tint)) == MH_OK &&
			MH_EnableHook(tint_target) == MH_OK;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			log_line("Disc light hook %s, chassis tint hook %s.", light_hooked ? "active" : "not available", tint_hooked ? "active" : "not available");
		}
		if (HANDLE thread = CreateThread(nullptr, 0, personal_disc_thread, nullptr, 0, nullptr))
			CloseHandle(thread);
		if (HANDLE thread = CreateThread(nullptr, 0, effect_thread, nullptr, 0, nullptr))
			CloseHandle(thread);
		if (s_log && s_debug)
			if (HANDLE thread = CreateThread(nullptr, 0, key_thread, nullptr, 0, nullptr))
				CloseHandle(thread);
		return 0;
	}

	HMODULE s_system_dinput8 = nullptr;

	// Plugin loader: loads bin\win10\plugins\<file> for every {"file": "..."} entry in echoloader.json, the list
	// EchoLoader uses, so plugins such as Echo Arcade work while dbgcore.dll is another patch (e.g. EchoRelay).
	// If EchoLoader is present too, loading again only adds a reference. DiscGlow.ini: LoadPlugins=0 turns it off.
	DWORD WINAPI load_plugins(void *)
	{
		const std::wstring dir(s_dir);
		if (GetPrivateProfileIntW(L"DiscGlow", L"LoadPlugins", 1, (dir + L"\\DiscGlow.ini").c_str()) == 0)
			return 0;
		FILE *file = nullptr;
		if (_wfopen_s(&file, (dir + L"\\echoloader.json").c_str(), L"rb") != 0 || file == nullptr)
			return 0;
		std::string json;
		char buffer[4096];
		for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
			json.append(buffer, read);
		std::fclose(file);

		for (size_t at = json.find("\"file\""); at != std::string::npos; at = json.find("\"file\"", at + 6))
		{
			const size_t open = json.find('"', json.find(':', at) + 1);
			const size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
			if (close == std::string::npos)
				break;
			const std::string name = json.substr(open + 1, close - open - 1);
			if (name.empty() || name.find_first_of("\\/:") != std::string::npos)
				continue; // Only plain file names inside the plugins folder
			const std::wstring path = dir + L"\\plugins\\" + std::wstring(name.begin(), name.end());
			const HMODULE plugin = LoadLibraryW(path.c_str());
			const std::lock_guard<std::mutex> lock(s_mutex);
			log_line("plugin %s: %s", name.c_str(), plugin != nullptr ? "loaded" : "could not be loaded");
		}
		return 0;
	}
}

// Forwarded DirectInput entry point (the only export the game uses)
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
	using create_fn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
	create_fn create = s_chain != nullptr ? reinterpret_cast<create_fn>(GetProcAddress(s_chain, "DirectInput8Create")) : nullptr;
	if (create == nullptr)
	{
		if (s_system_dinput8 == nullptr)
		{
			wchar_t path[MAX_PATH];
			GetSystemDirectoryW(path, MAX_PATH);
			wcscat_s(path, L"\\dinput8.dll");
			s_system_dinput8 = LoadLibraryW(path);
		}
		create = s_system_dinput8 != nullptr ? reinterpret_cast<create_fn>(GetProcAddress(s_system_dinput8, "DirectInput8Create")) : nullptr;
	}
	return create != nullptr ? create(instance, version, riid, out, outer) : E_FAIL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		GetModuleFileNameW(module, s_dir, MAX_PATH);
		if (wchar_t *slash = wcsrchr(s_dir, L'\\'))
			*slash = L'\0';
		// Another mod that was installed as dinput8.dll (e.g. ReShade) is loaded now, as early as it would have been
		const std::wstring chain = std::wstring(s_dir) + L"\\dinput8.chain.dll";
		if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES)
			s_chain = LoadLibraryW(chain.c_str());
		if (s_chain == module) // DiscGlow itself (e.g. chained by another loader): forwarding to it would loop
		{
			FreeLibrary(s_chain);
			s_chain = nullptr;
		}
		// Hooking from DllMain would run under the loader lock, so do it on a separate thread
		if (HANDLE thread = CreateThread(nullptr, 0, install, nullptr, 0, nullptr))
			CloseHandle(thread);
		if (HANDLE thread = CreateThread(nullptr, 0, load_plugins, nullptr, 0, nullptr))
			CloseHandle(thread);
	}
	return TRUE;
}
