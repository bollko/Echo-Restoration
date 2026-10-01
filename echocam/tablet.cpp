/*
 * Where the social tablet really is. The game places the tablet from the arm each frame (not rigidly from the controller),
 * so the side panel follows the tablet's own touch buttons instead.
 *
 * CR15ButtonInteractCS update (FUN_14092f3f0, also hooked by Echo Arcade) computes every touch button's world position:
 *   +0xd0 -> resource; *(resource) = rows (0x128 bytes each), +0x30 row count. Row +0x08 actor, +0x80 / +0x84 the
 *   button's centre in metres from the canvas's top-left corner (x right, y down).
 *   +0x10c u16 button count, +0xf0 u16 handle list, +0xe0 (u16 slot, u16 generation) per handle,
 *   +0x110 button instances (400 bytes each): +0x00 u16 row, +0x118 world position (x, y, z).
 * The tablet's buttons (actor 0x6c1f6ff04e070923) give its plane by a least-squares fit:
 *   world = top_left + x * right + y * down.
 *
 * Hiding the tablet from the camera only: each level's render scene (gamespace +0x60 -> +0x1c8) keeps a 256-bit visibility
 * mask per viewport (+0x948, 0x20 each, count +0x950). Scene sets (+0xa8 -> +0xc88, 0x30 each, mask at +0x10) are found by
 * name in two sorted tables (+0xa8 -> +0xc48 / count +0xc78, and +0xcc0 / +0xcf0). The engine's set-visibility messages use
 * FUN_1404eb700 / FUN_1404e4bc0 (hide / show, first table) and FUN_1404f1b60 / FUN_1404f1930 (second table), each
 * (gamespace, set, viewport bits). The tablet's set is its sub-level, r14_glb_global_mp. Viewport 1 is the camera's.
 */
#include "tablet.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include "../discglow/include/MinHook.h"

namespace
{
	constexpr uintptr_t BUTTON_UPDATE_RVA = 0x92f3f0;
	constexpr uint64_t TABLET_ACTOR = 0x6c1f6ff04e070923;
	constexpr size_t ROW_SIZE = 0x128, INSTANCE_SIZE = 400;

	using update_fn = void(__fastcall *)(void *cs);
	using set_visibility_fn = void(__fastcall *)(uintptr_t gamespace, unsigned set, unsigned viewports, uint64_t unused);
	constexpr uintptr_t HIDE_SET_RVA[2] = { 0x4eb700, 0x4f1b60 }, SHOW_SET_RVA[2] = { 0x4e4bc0, 0x4f1930 };
	constexpr uint8_t SET_VISIBILITY_BYTES[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48 };
	constexpr uint64_t TABLET_SET = 0x3f9915d3001dc28e; // sym("r14_glb_global_mp")
	constexpr unsigned CAMERA_VIEWPORT = 1u << 1;
	set_visibility_fn s_hide_set[2] = {}, s_show_set[2] = {};
	std::atomic<bool> s_want_hidden{ false };
	bool s_hidden = false;        // game thread
	uintptr_t s_hidden_in = 0;    // the gamespace it was hidden in
	unsigned s_hidden_set = 0;
	int s_hidden_table = 0;
	bool s_logged_sets = false;
	update_fn s_original = nullptr;
	void (*s_log)(const char *, ...) = nullptr;

	std::mutex s_mutex;
	tablet::PanelPose s_pose{};
	ULONGLONG s_pose_time = 0;
	bool s_logged = false;
	float s_head[3] = {}, s_scale = 1.3f, s_nudge[2] = { 0.0175f, 0.0125f };
	bool s_have_head = false;

	template <typename T>
	bool read(uintptr_t address, T &value)
	{
		__try
		{
			std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	struct Sums
	{
		double n = 0, x = 0, y = 0, xx = 0, xy = 0, yy = 0, w[3] = {}, xw[3] = {}, yw[3] = {};
		float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
		void add(float lx, float ly, const float *world)
		{
			n += 1;
			x += lx;
			y += ly;
			xx += double(lx) * lx;
			xy += double(lx) * ly;
			yy += double(ly) * ly;
			for (int c = 0; c < 3; ++c)
			{
				w[c] += world[c];
				xw[c] += double(lx) * world[c];
				yw[c] += double(ly) * world[c];
			}
			min_x = (std::min)(min_x, lx);
			max_x = (std::max)(max_x, lx);
			min_y = (std::min)(min_y, ly);
			max_y = (std::max)(max_y, ly);
		}
	};

	// Reads the tablet's buttons; false when they are not there (another button system, or no tablet)
	bool collect(uintptr_t cs, Sums &sums)
	{
		uintptr_t resource = 0, rows = 0, handles = 0, slots = 0, instances = 0;
		uint64_t row_count = 0;
		uint16_t count = 0;
		if (!read(cs + 0xd0, resource) || !read(resource, rows) || !read(resource + 0x30, row_count) || !read(cs + 0x10c, count) ||
			!read(cs + 0xf0, handles) || !read(cs + 0xe0, slots) || !read(cs + 0x110, instances) || !rows || !handles || !slots || !instances)
			return false;
		for (uint16_t i = 0; i < count; ++i)
		{
			uint16_t handle = 0, slot = 0, row = 0;
			if (!read(handles + i * 2ull, handle) || !read(slots + handle * 4ull, slot))
				return false;
			const uintptr_t instance = instances + slot * INSTANCE_SIZE;
			uint64_t actor = 0;
			float local[2], world[3];
			if (!read(instance, row) || row >= row_count || !read(rows + row * ROW_SIZE + 8, actor))
				return false;
			if (actor != TABLET_ACTOR)
				continue;
			if (!read(rows + row * ROW_SIZE + 0x80, local) || !read(instance + 0x118, world))
				return false;
			if (!std::isfinite(world[0]) || (world[0] == 0 && world[1] == 0 && world[2] == 0))
				continue;
			sums.add(local[0], local[1], world);
		}
		return sums.n >= 3;
	}

	bool solve3(const double m[3][3], const double b[3], double out[3])
	{
		const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		if (std::fabs(det) < 1e-12)
			return false;
		for (int k = 0; k < 3; ++k)
		{
			double t[3][3];
			std::memcpy(t, m, sizeof(t));
			for (int r = 0; r < 3; ++r)
				t[r][k] = b[r];
			out[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
				t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
		}
		return true;
	}

	void normalise(float *v)
	{
		const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (l > 1e-6f)
			for (int c = 0; c < 3; ++c)
				v[c] /= l;
	}

	void fit(uintptr_t cs)
	{
		Sums s;
		if (!collect(cs, s))
			return;
		const double m[3][3] = { { s.n, s.x, s.y }, { s.x, s.xx, s.xy }, { s.y, s.xy, s.yy } };
		float origin[3], right[3], down[3];
		for (int c = 0; c < 3; ++c)
		{
			const double b[3] = { s.w[c], s.xw[c], s.yw[c] };
			double r[3];
			if (!solve3(m, b, r))
				return;
			origin[c] = float(r[0]);
			right[c] = float(r[1]);
			down[c] = float(r[2]);
		}
		tablet::PanelPose p;
		std::memcpy(p.right, right, sizeof(right));
		normalise(p.right);
		for (int c = 0; c < 3; ++c)
			p.up[c] = -down[c];
		// up square to right
		const float along = p.up[0] * p.right[0] + p.up[1] * p.right[1] + p.up[2] * p.right[2];
		for (int c = 0; c < 3; ++c)
			p.up[c] -= along * p.right[c];
		normalise(p.up);
		p.out[0] = p.right[1] * p.up[2] - p.right[2] * p.up[1];
		p.out[1] = p.right[2] * p.up[0] - p.right[0] * p.up[2];
		p.out[2] = p.right[0] * p.up[1] - p.right[1] * p.up[0];
		// The tablet faces the player: if "out" points away from the head, the layout's y runs up rather than down
		float head[3], panel_scale, nudge[2];
		bool have_head;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			std::memcpy(head, s_head, sizeof(head));
			have_head = s_have_head;
			panel_scale = s_scale;
			nudge[0] = s_nudge[0];
			nudge[1] = s_nudge[1];
		}
		const float to_head[3] = { head[0] - origin[0], head[1] - origin[1], head[2] - origin[2] };
		const bool flipped = have_head && to_head[0] * p.out[0] + to_head[1] * p.out[1] + to_head[2] * p.out[2] < 0;
		if (flipped)
			for (int c = 0; c < 3; ++c)
			{
				p.up[c] = -p.up[c];
				p.out[c] = -p.out[c];
			}

		// The buttons span the tablet: its right edge is a little past the rightmost ones, its height their span
		const float scale = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]); // metres per layout metre (1 unless stretched)
		const float edge_x = s.max_x + 0.012f, gap = 0.01f;
		p.height = (s.max_y - s.min_y + 0.024f) * scale * panel_scale;
		p.width = p.height * 3 / 4;
		const float mid_y = (s.min_y + s.max_y) / 2;
		for (int c = 0; c < 3; ++c)
			p.centre[c] = origin[c] + right[c] * edge_x + down[c] * mid_y + p.right[c] * (gap + p.width / 2 + nudge[0]) + p.up[c] * nudge[1];
		if (!std::isfinite(p.centre[0]) || p.height <= 0.05f || p.height > 1.0f)
			return;
		const std::lock_guard<std::mutex> lock(s_mutex);
		s_pose = p;
		s_pose_time = GetTickCount64();
		if (!s_logged && s_log)
		{
			s_logged = true;
			s_log("tablet: found from %d buttons: span x %.3f..%.3f y %.3f..%.3f m, panel %.3f x %.3f m%s", int(s.n), s.min_x, s.max_x, s.min_y,
				s.max_y, p.width, p.height, flipped ? " (layout y runs up)" : "");
		}
	}

	// The tablet's scene set: (table 0 or 1, index), or false
	bool find_tablet_set(uintptr_t gamespace, int &table, unsigned &index)
	{
		uintptr_t scene = 0, render = 0, level = 0;
		if (!read(gamespace + 0x60, scene) || !read(scene + 0x1c8, render) || !read(render + 0xa8, level) || !level)
			return false;
		for (int t = 0; t < 2; ++t)
		{
			uintptr_t entries = 0;
			uint64_t count = 0;
			if (!read(level + (t ? 0xcc0 : 0xc48), entries) || !read(level + (t ? 0xcf0 : 0xc78), count) || !entries || count > 4096)
				continue;
			if (!s_logged_sets && s_log)
			{
				char line[1024] = "";
				size_t used = 0;
				for (uint64_t i = 0; i < count && used + 40 < sizeof(line); ++i)
				{
					uint64_t name = 0;
					uint32_t at = 0;
					if (read(entries + i * 16, name) && read(entries + i * 16 + 8, at))
						used += snprintf(line + used, sizeof(line) - used, " %016llx=%u", static_cast<unsigned long long>(name), at);
				}
				s_log("tablet: scene set table %d (%llu):%s", t, static_cast<unsigned long long>(count), line);
			}
			for (uint64_t i = 0; i < count; ++i)
			{
				uint64_t name = 0;
				uint32_t at = 0;
				if (read(entries + i * 16, name) && name == TABLET_SET && read(entries + i * 16 + 8, at))
				{
					table = t;
					index = at;
					return true;
				}
			}
		}
		s_logged_sets = true;
		return false;
	}

	// Game thread: hides or shows the tablet's scene set in the camera's viewport as asked
	void update_visibility(uintptr_t cs)
	{
		const bool want = s_want_hidden;
		if (want == s_hidden)
			return;
		if (!want)
		{
			if (s_hidden_in && s_show_set[s_hidden_table])
				s_show_set[s_hidden_table](s_hidden_in, s_hidden_set, CAMERA_VIEWPORT, 0);
			s_hidden = false;
			s_hidden_in = 0;
			if (s_log)
				s_log("tablet: shown in the camera again");
			return;
		}
		uintptr_t gamespace = 0;
		int table = 0;
		unsigned index = 0;
		if (!read(cs + 0x80, gamespace) || !gamespace || !find_tablet_set(gamespace, table, index) || !s_hide_set[table])
		{
			s_hidden = true; // not found: do not search every frame (logged once)
			if (s_log)
				s_log("tablet: its scene set was not found; it stays visible in the camera");
			return;
		}
		s_hide_set[table](gamespace, index, CAMERA_VIEWPORT, 0);
		s_hidden = true;
		s_hidden_in = gamespace;
		s_hidden_set = index;
		s_hidden_table = table;
		if (s_log)
			s_log("tablet: hidden from the camera (set table %d index %u)", table, index);
	}

	void __fastcall hooked_update(void *cs)
	{
		s_original(cs);
		fit(reinterpret_cast<uintptr_t>(cs));
		__try
		{
			update_visibility(reinterpret_cast<uintptr_t>(cs));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			s_hidden = s_want_hidden;
		}
	}
}

namespace tablet
{
	bool panel_pose(PanelPose &pose)
	{
		const std::lock_guard<std::mutex> lock(s_mutex);
		if (s_pose_time == 0 || GetTickCount64() - s_pose_time > 500)
			return false;
		pose = s_pose;
		return true;
	}

	void set_head(const float *world_position)
	{
		const std::lock_guard<std::mutex> lock(s_mutex);
		std::memcpy(s_head, world_position, sizeof(s_head));
		s_have_head = true;
	}

	void set_nudge(float right, float up)
	{
		const std::lock_guard<std::mutex> lock(s_mutex);
		s_nudge[0] = right;
		s_nudge[1] = up;
	}

	void hide_in_camera(bool hide)
	{
		s_want_hidden = hide;
	}

	void set_scale(float scale)
	{
		const std::lock_guard<std::mutex> lock(s_mutex);
		s_scale = (std::max)(0.5f, (std::min)(scale, 3.0f));
	}

	bool start(unsigned char *exe, void (*log)(const char *format, ...))
	{
		s_log = log;
		for (int t = 0; t < 2; ++t)
			if (std::memcmp(exe + HIDE_SET_RVA[t], SET_VISIBILITY_BYTES, sizeof(SET_VISIBILITY_BYTES)) == 0 &&
				std::memcmp(exe + SHOW_SET_RVA[t], SET_VISIBILITY_BYTES, sizeof(SET_VISIBILITY_BYTES)) == 0)
			{
				s_hide_set[t] = reinterpret_cast<set_visibility_fn>(exe + HIDE_SET_RVA[t]);
				s_show_set[t] = reinterpret_cast<set_visibility_fn>(exe + SHOW_SET_RVA[t]);
			}
		// Same prologue check as Echo Arcade's runtime: if it already hooked the function, chain onto its jump
		void *target = exe + BUTTON_UPDATE_RVA;
		const bool ok = MH_CreateHook(target, reinterpret_cast<void *>(&hooked_update), reinterpret_cast<void **>(&s_original)) == MH_OK &&
			MH_EnableHook(target) == MH_OK;
		log("tablet: touch-button hook %s", ok ? "active" : "not available (the panel follows the controller)");
		return ok;
	}
}
