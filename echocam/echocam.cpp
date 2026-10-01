/*
 * EchoCam: a camera from any point of view for Echo VR (final PC build), using the game's own second viewport.
 *
 * Started with -capturevp2, the game renders a second viewport to the desktop window. Every frame
 * FUN_14011f9c0 (UpdateCaptureCamera) places that viewport's camera: it reads the head's transform from a node
 * (FUN_140629c30) and calls the camera's SetPosition (vtable +0x68) and SetRotation (vtable +0x70), then sets its
 * FOV from the graphics_capture settings' capturefov, and submits the view. EchoCam swaps the head transform that the
 * function reads (FUN_140629c30) for the camera pose it wants.
 *
 * Hand mode puts the camera on a controller (the tablet hand). FUN_140629c30 places the head's play-space pose
 * (FUN_14072cd50 / FUN_14072cc10) on the capture node; EchoCam does the same with the game's controller getters and its
 * own vector and quaternion functions.
 *
 * Loaded as a plugin (bin\win10\plugins\EchoCam.dll, listed in echoloader.json). EchoCam.ini next to the DLL,
 * section [EchoCam] (re-read while the game runs):
 *   Mode=head             head (the game's own camera), third_person, hand (the CAMERA tile sets hand while open)
 *   Hand=left             hand mode: left or right controller
 *   HandOffset=0 0 0      hand mode: camera position in the controller's space (metres, x right, y up, z back)
 *   HandYaw=0             hand mode: camera turn in degrees (180 = looks back at you, a selfie)
 *   HandPitch=0           hand mode: camera tilt in degrees
 *   OnPanel=1             hand mode, with the side panel's hand: the camera is the panel's, like a phone's (HandYaw 0 looks
 *                         out of the back of the panel, 180 is the selfie camera; HandOffset z = reach in metres)
 *   Freeze=0              1 keeps the camera where it was when frozen (a tripod), until 0
 *   HideTablet=1          hides the tablet in the camera's picture while the camera is in use (the headset still shows it)
 *   Smoothing=0           0 (off) .. 1: the camera eases towards where it should be, like a gimbal (up to ~0.6 s lag)
 *   Distance=2            metres behind the head (third_person)
 *   Height=0.4            metres above the head (third_person)
 *   Log=1                 write EchoCam.log
 * F10 writes the current head and hand poses to EchoCam.log.
 */

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <string>
#include "../discglow/include/MinHook.h"
#include "panel.h"
#include "tablet.h"
#include "bridge.h"

namespace
{
	// FUN_14011f9c0: void UpdateCaptureCamera(game)
	constexpr uintptr_t UPDATE_CAMERA_RVA = 0x11f9c0;
	constexpr uint8_t UPDATE_CAMERA_BYTES[] = { 0x40, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x91, 0xe0, 0x7a, 0x00, 0x00 };
	// FUN_140629c30: float* GetNodeTransform(node reference, float out[8]): rotation quaternion xyzw, then position xyz 1
	constexpr uintptr_t GET_TRANSFORM_RVA = 0x629c30;
	constexpr uint8_t GET_TRANSFORM_BYTES[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00 };
	constexpr uintptr_t RENDERER_RVA = 0x20a0470;      // DAT_1420a0470: renderer / game world
	constexpr uintptr_t HEAD_NODE = 0x2ce80;           // node reference the capture camera follows
	constexpr uintptr_t CAPTURE_CAMERA = 0x2ce88;      // the second viewport's camera
	constexpr uintptr_t GAME_FLAGS = 0x7ae0;           // bit 29: -capturevp2
	// Node reference: *(ref + 0x70) + 0x28 = node array (0x30 bytes per node: quaternion, position), *(u16)(ref + 0x78) = index
	constexpr uintptr_t NODE_SET = 0x70, NODE_INDEX = 0x78, NODE_ARRAY = 0x28, NODE_SIZE = 0x30;
	// Controller poses in play space (valid while both are set): FUN_14072c6d0 / FUN_14072de90 (out quaternion, 1),
	// FUN_14072c780 / FUN_14072df40 (out position, 1) for the left / right controller
	constexpr uintptr_t TRACKING_SESSION_RVA = 0x20c77a0, HAND_READY_RVA = 0x20223cc;
	constexpr uintptr_t HAND_ROTATION_RVA[2] = { 0x72c6d0, 0x72de90 }, HAND_POSITION_RVA[2] = { 0x72c780, 0x72df40 };
	constexpr uintptr_t PLAY_SPACE_ROTATION_RVA = 0x20223f8, HEAD_OFFSET_RVA = 0x20c7790; // see FUN_14072cd50
	constexpr uint8_t HAND_GETTER_BYTES[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x83, 0x3d };
	// FUN_1400f93b0(out, vector, quaternion): rotates; FUN_1400f94e0(out, a, b): quaternion product a * b
	constexpr uintptr_t ROTATE_RVA = 0xf93b0, MULTIPLY_RVA = 0xf94e0;

	using update_camera_fn = void(__fastcall *)(uintptr_t game);
	using get_transform_fn = float *(__fastcall *)(uintptr_t node, float *out);
	using camera_set_fn = void(__fastcall *)(uintptr_t camera, const float *value);
	using rotate_fn = float *(__fastcall *)(float *out, const float *vector, const float *quaternion);
	using multiply_fn = float *(__fastcall *)(float *out, const float *a, const float *b);

	update_camera_fn s_original = nullptr;
	rotate_fn s_rotate = nullptr;
	multiply_fn s_multiply = nullptr;
	using hand_getter_fn = float *(__fastcall *)(float *out, int current);
	hand_getter_fn s_hand_rotation[2] = {}, s_hand_position[2] = {};
	uintptr_t s_base = 0;
	wchar_t s_dir[MAX_PATH] = L"";
	FILE *s_log = nullptr;
	std::mutex s_mutex;

	// Settings (guarded by s_mutex)
	enum mode { MODE_HEAD, MODE_THIRD_PERSON, MODE_HAND };
	int s_mode = MODE_HEAD, s_hand = 0;
	float s_distance = 2.0f, s_height = 0.4f;
	float s_hand_offset[3] = {};
	alignas(16) float s_hand_turn[4] = { 0, 0, 0, 1 }; // From HandYaw and HandPitch
	bool s_logged_first = false, s_log_poses = false, s_freeze = false, s_on_panel = true;
	float s_smoothing = 0;
	bool s_hide_tablet = true;
	// Smoothed pose (camera thread only)
	float s_smooth_pose[8];
	bool s_have_smooth = false;
	LARGE_INTEGER s_smooth_time{};
	// The pose kept while frozen (camera thread only)
	alignas(16) float s_frozen_pose[8];
	bool s_have_frozen = false;

	void log_line(const char *format, ...)
	{
		if (s_log == nullptr)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::fprintf(s_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, format);
		std::vfprintf(s_log, format, args);
		va_end(args);
		std::fputc('\n', s_log);
		std::fflush(s_log);
	}

	// For other threads (the side panel): log_line under the settings lock
	void locked_log(const char *format, ...)
	{
		if (s_log == nullptr)
			return;
		char line[512];
		va_list args;
		va_start(args, format);
		vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		const std::lock_guard<std::mutex> lock(s_mutex);
		log_line("%s", line);
	}

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

	std::wstring ini_path()
	{
		return std::wstring(s_dir) + L"\\EchoCam.ini";
	}

	float ini_float(const wchar_t *key, float fallback)
	{
		wchar_t value[64];
		GetPrivateProfileStringW(L"EchoCam", key, L"", value, 64, ini_path().c_str());
		float result;
		return swscanf_s(value, L"%f", &result) == 1 ? result : fallback;
	}

	void load_config()
	{
		wchar_t value[64], hand[16], offset[64];
		GetPrivateProfileStringW(L"EchoCam", L"Mode", L"head", value, 64, ini_path().c_str());
		GetPrivateProfileStringW(L"EchoCam", L"Hand", L"left", hand, 16, ini_path().c_str());
		GetPrivateProfileStringW(L"EchoCam", L"HandOffset", L"0 0 0", offset, 64, ini_path().c_str());
		const float yaw = ini_float(L"HandYaw", 0) * 0.0174532925f, pitch = ini_float(L"HandPitch", 0) * 0.0174532925f;
		const std::lock_guard<std::mutex> lock(s_mutex);
		s_mode = _wcsicmp(value, L"head") == 0 ? MODE_HEAD : _wcsicmp(value, L"third_person") == 0 ? MODE_THIRD_PERSON : MODE_HAND;
		s_hand = _wcsicmp(hand, L"right") == 0 ? 1 : 0;
		if (swscanf_s(offset, L"%f %f %f", &s_hand_offset[0], &s_hand_offset[1], &s_hand_offset[2]) != 3)
			s_hand_offset[0] = s_hand_offset[1] = s_hand_offset[2] = 0;
		// Yaw about y, then pitch about x: q = yaw * pitch
		const float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
		s_hand_turn[0] = cy * sp;
		s_hand_turn[1] = sy * cp;
		s_hand_turn[2] = -sy * sp;
		s_hand_turn[3] = cy * cp;
		s_distance = ini_float(L"Distance", 2.0f);
		s_height = ini_float(L"Height", 0.4f);
		s_freeze = GetPrivateProfileIntW(L"EchoCam", L"Freeze", 0, ini_path().c_str()) != 0;
		s_smoothing = (std::max)(0.0f, (std::min)(ini_float(L"Smoothing", 0), 1.0f));
		s_hide_tablet = GetPrivateProfileIntW(L"EchoCam", L"HideTablet", 1, ini_path().c_str()) != 0;
		tablet::hide_in_camera(s_hide_tablet && s_mode != MODE_HEAD);
		s_on_panel = GetPrivateProfileIntW(L"EchoCam", L"OnPanel", 1, ini_path().c_str()) != 0;
	}

	// Rotates v by the unit quaternion q (x, y, z, w)
	void rotate(const float *q, const float *v, float *out)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		// t = 2 * cross(q.xyz, v); out = v + w * t + cross(q.xyz, t)
		const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
		out[0] = v[0] + w * tx + (y * tz - z * ty);
		out[1] = v[1] + w * ty + (z * tx - x * tz);
		out[2] = v[2] + w * tz + (x * ty - y * tx);
	}

	void quat_multiply(const float *a, const float *b, float *out)
	{
		const float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
			a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
		std::memcpy(out, r, sizeof(r));
	}

	// The side panel's camera, like a phone's: the panel is held (panel::layout) in Oculus controller space; the game's
	// controller axes are Oculus' turned half way round y (x and z flipped), so offsets and rotations are flipped the same way.
	// The camera looks along its local -Z; the panel faces its viewer along its Oculus +Z, which is the game's -Z.
	void panel_camera(const float *grip, const float *turn, float reach, float *position, float *rotation)
	{
		int panel_hand;
		float offset[3], turn_ovr[4], width;
		panel::layout(panel_hand, offset, turn_ovr, width);
		const float turn_game[4] = { -turn_ovr[0], turn_ovr[1], -turn_ovr[2], turn_ovr[3] };
		float panel_q[4];
		quat_multiply(grip, turn_game, panel_q);

		const float offset_game[3] = { -offset[0], offset[1], -offset[2] };
		float centre[3];
		rotate(grip, offset_game, centre);
		// Lens near the top edge, pushed away from the viewer by the reach beyond the stick's shortest setting
		const float lens[3] = { 0, width * 4 / 3 / 2 - 0.02f, (std::max)(0.0f, reach - 0.25f) };
		float lens_world[3];
		rotate(panel_q, lens, lens_world);
		for (int c = 0; c < 3; ++c)
			position[c] = grip[4 + c] + centre[c] + lens_world[c];
		// Out of the back of the panel = half a turn about y from the panel's own facing; the selfie turn undoes it
		const float back[4] = { 0, 1, 0, 0 };
		float q[4];
		quat_multiply(panel_q, back, q);
		quat_multiply(q, turn, rotation);
	}

	// World pose of controller 'hand' (0 left, 1 right), placed like the head on 'node': out[0..3] quaternion, out[4..6] position.
	// The game's controller getters give the pose in play space (like FUN_14072cd50 / FUN_14072cc10 for the head).
	bool hand_pose(uintptr_t node, int hand, float *out)
	{
		uintptr_t session = 0, set = 0, array = 0;
		uint32_t ready = 0;
		uint16_t index = 0;
		alignas(16) float node_pose[8];
		if (!safe_read(s_base + TRACKING_SESSION_RVA, session) || !safe_read(s_base + HAND_READY_RVA, ready) || session == 0 || ready == 0 ||
			!safe_read(node + NODE_SET, set) || !safe_read(set + NODE_ARRAY, array) || !safe_read(node + NODE_INDEX, index) ||
			!safe_copy(node_pose, reinterpret_cast<const void *>(array + index * NODE_SIZE), sizeof(node_pose)))
			return false;
		alignas(16) float local_p[4] = {}, local_q[4] = {};
		s_hand_rotation[hand](local_q, 1);
		s_hand_position[hand](local_p, 1);
		alignas(16) float node_q[4] = { node_pose[0], node_pose[1], node_pose[2], node_pose[3] };
		alignas(16) float world_p[4], world_q[4];
		s_rotate(world_p, local_p, node_q);
		s_multiply(world_q, node_q, local_q);
		std::memcpy(out, world_q, 4 * sizeof(float));
		out[4] = node_pose[4] + world_p[0];
		out[5] = node_pose[5] + world_p[1];
		out[6] = node_pose[6] + world_p[2];
		return true;
	}

	// Set while the game places the capture camera, on its thread: then the head transform it reads is swapped
	thread_local bool t_placing_camera = false;
	get_transform_fn s_original_get_transform = nullptr;

	bool tablet_camera(const float *turn, float reach, float *position, float *rotation);

	// Eases 'pose' (rotation, position) towards its target over time: amount 0 = off, 1 = a time constant of ~0.6 s.
	// Big jumps (a new mode, a respawn) are taken at once.
	void smooth(float *pose, float amount)
	{
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		const float dt = s_have_smooth ? float(now.QuadPart - s_smooth_time.QuadPart) / float(frequency.QuadPart) : 0;
		s_smooth_time = now;
		const float jump = std::sqrt((pose[4] - s_smooth_pose[4]) * (pose[4] - s_smooth_pose[4]) + (pose[5] - s_smooth_pose[5]) * (pose[5] - s_smooth_pose[5]) +
			(pose[6] - s_smooth_pose[6]) * (pose[6] - s_smooth_pose[6]));
		if (amount <= 0 || !s_have_smooth || dt <= 0 || dt > 0.5f || jump > 2.0f)
		{
			std::memcpy(s_smooth_pose, pose, sizeof(s_smooth_pose));
			s_have_smooth = true;
			return;
		}
		const float k = 1 - std::exp(-dt / (amount * 0.6f));
		for (int c = 4; c < 7; ++c)
			s_smooth_pose[c] += (pose[c] - s_smooth_pose[c]) * k;
		// Rotation: normalised lerp on the same hemisphere
		const float d = s_smooth_pose[0] * pose[0] + s_smooth_pose[1] * pose[1] + s_smooth_pose[2] * pose[2] + s_smooth_pose[3] * pose[3];
		const float sign = d < 0 ? -1.0f : 1.0f;
		float length = 0;
		for (int c = 0; c < 4; ++c)
		{
			s_smooth_pose[c] += (pose[c] * sign - s_smooth_pose[c]) * k;
			length += s_smooth_pose[c] * s_smooth_pose[c];
		}
		length = std::sqrt(length);
		for (int c = 0; c < 4; ++c)
			s_smooth_pose[c] /= length;
		std::memcpy(pose, s_smooth_pose, 7 * sizeof(float));
	}

	// Replaces 'pose' (the head: rotation, then position) with the camera pose for the current mode
	void camera_pose(uintptr_t node, float *pose)
	{
		int mode, hand;
		float distance, height, hand_offset[3];
		alignas(16) float hand_turn[4];
		bool log_poses, freeze, on_panel;
		float smoothing;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			mode = s_mode;
			hand = s_hand;
			distance = s_distance;
			height = s_height;
			std::memcpy(hand_offset, s_hand_offset, sizeof(hand_offset));
			std::memcpy(hand_turn, s_hand_turn, sizeof(hand_turn));
			log_poses = s_log_poses;
			s_log_poses = false;
			freeze = s_freeze;
			on_panel = s_on_panel;
			smoothing = s_smoothing;
		}

		alignas(16) float head[8];
		std::memcpy(head, pose, sizeof(head));
		tablet::set_head(head + 4);
		alignas(16) float hands[2][8] = {};
		const bool hands_ok = hand_pose(node, 0, hands[0]) && hand_pose(node, 1, hands[1]);
		if (log_poses)
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			log_line("head  pos %.3f %.3f %.3f rot %.3f %.3f %.3f %.3f", head[4], head[5], head[6], head[0], head[1], head[2], head[3]);
			for (int h = 0; h < 2 && hands_ok; ++h)
				log_line("%s pos %.3f %.3f %.3f rot %.3f %.3f %.3f %.3f", h == 0 ? "left " : "right", hands[h][4], hands[h][5], hands[h][6],
					hands[h][0], hands[h][1], hands[h][2], hands[h][3]);
			if (!hands_ok)
				log_line("hand poses not available");
		}
		if (mode == MODE_HEAD || !freeze)
			s_have_frozen = false;
		if (mode != MODE_HEAD && freeze && s_have_frozen)
		{
			std::memcpy(pose, s_frozen_pose, sizeof(s_frozen_pose));
			return;
		}
		if (mode == MODE_HEAD || (mode == MODE_HAND && !hands_ok))
			return;

		alignas(16) float position[3];
		alignas(16) float rotation[4];
		int panel_hand;
		float unused_offset[3], unused_rotation[4], unused_width;
		panel::layout(panel_hand, unused_offset, unused_rotation, unused_width);
		if (mode == MODE_HAND && on_panel && hand == panel_hand && tablet_camera(hand_turn, hand_offset[2], position, rotation))
			;
		else if (mode == MODE_HAND && on_panel && hand == panel_hand)
			panel_camera(hands[hand], hand_turn, hand_offset[2], position, rotation);
		else if (mode == MODE_HAND)
		{
			const float *grip = hands[hand];
			float offset[3];
			rotate(grip, hand_offset, offset);
			for (int c = 0; c < 3; ++c)
				position[c] = grip[4 + c] + offset[c];
			alignas(16) float hand_q[4] = { grip[0], grip[1], grip[2], grip[3] };
			s_multiply(rotation, hand_q, hand_turn);
		}
		else
		{
			// Behind the head: the camera looks along its local -Z, so +Z is behind it
			const float back[3] = { 0, 0, distance };
			float offset[3];
			rotate(head, back, offset);
			position[0] = head[4] + offset[0];
			position[1] = head[5] + offset[1] + height;
			position[2] = head[6] + offset[2];
			std::memcpy(rotation, head, sizeof(rotation));
		}
		std::memcpy(pose, rotation, sizeof(rotation));
		std::memcpy(pose + 4, position, sizeof(position));
		smooth(pose, smoothing);
		if (freeze)
		{
			std::memcpy(s_frozen_pose, pose, sizeof(s_frozen_pose));
			s_have_frozen = true;
		}

		if (!s_logged_first)
		{
			s_logged_first = true;
			const std::lock_guard<std::mutex> lock(s_mutex);
			log_line("capture camera placed (mode %s, hand poses %s): head %.2f %.2f %.2f -> %.2f %.2f %.2f", mode == MODE_HAND ? "hand" : "third_person",
				hands_ok ? "available" : "not available", head[4], head[5], head[6], position[0], position[1], position[2]);
		}
	}

	// FUN_140629c30: while the capture camera is placed, returns the camera pose instead of the head's
	float *__fastcall hooked_get_transform(uintptr_t node, float *out)
	{
		float *const result = s_original_get_transform(node, out);
		if (t_placing_camera)
		{
			t_placing_camera = false; // Only the camera's own read (the function reads the node once)
			camera_pose(node, out);
		}
		return result;
	}

	// The game sets the camera from the head and submits the view in the same call, so moving the camera afterwards would
	// never be drawn: the head transform it reads is swapped instead (hooked_get_transform).
	void conj_rotate(const float *q, const float *v, float *out)
	{
		const float c[4] = { -q[0], -q[1], -q[2], q[3] };
		rotate(c, v, out);
	}

	// The tablet's side panel as a phone camera (game world): selfie looks out of the panel at you, front out of its back
	bool tablet_camera(const float *turn, float reach, float *position, float *rotation)
	{
		tablet::PanelPose p;
		if (!tablet::panel_pose(p))
			return false;
		const bool selfie = std::fabs(turn[1]) > 0.5f; // HandYaw 180
		const float away = (std::max)(0.0f, reach - 0.25f);
		for (int c = 0; c < 3; ++c)
			position[c] = p.centre[c] + p.up[c] * (p.height / 2 - 0.02f) - p.out[c] * away;
		// The camera looks along its local -Z
		float right[3], back[3];
		for (int c = 0; c < 3; ++c)
		{
			right[c] = selfie ? -p.right[c] : p.right[c];
			back[c] = selfie ? -p.out[c] : p.out[c];
		}
		bridge::quat_from_axes(right, p.up, back, rotation);
		return true;
	}

	void __fastcall hooked_update_camera(uintptr_t game)
	{
		uint64_t flags = 0;
		t_placing_camera = safe_read(game + GAME_FLAGS, flags) && ((flags >> 29) & 1) != 0;
		s_original(game);
		t_placing_camera = false;
	}

	DWORD WINAPI key_thread(void *)
	{
		bool was_down = false;
		FILETIME last_ini = {};
		for (;;)
		{
			Sleep(50);
			const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
			if (down && !was_down)
			{
				const std::lock_guard<std::mutex> lock(s_mutex);
				s_log_poses = true; // Logged by the next camera update
			}
			was_down = down;

			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &data) && CompareFileTime(&data.ftLastWriteTime, &last_ini) != 0)
			{
				last_ini = data.ftLastWriteTime;
				load_config();
				panel::reload();
			}
		}
	}

	DWORD WINAPI install(void *)
	{
		load_config();
		if (GetPrivateProfileIntW(L"EchoCam", L"Log", 1, ini_path().c_str()) != 0)
			s_log = _wfsopen((std::wstring(s_dir) + L"\\EchoCam.log").c_str(), L"w", _SH_DENYWR);

		s_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		void *const target = reinterpret_cast<void *>(s_base + UPDATE_CAMERA_RVA);
		void *const get_transform = reinterpret_cast<void *>(s_base + GET_TRANSFORM_RVA);
		if (std::memcmp(target, UPDATE_CAMERA_BYTES, sizeof(UPDATE_CAMERA_BYTES)) != 0 ||
			std::memcmp(get_transform, GET_TRANSFORM_BYTES, sizeof(GET_TRANSFORM_BYTES)) != 0)
		{
			log_line("Game version not recognised, nothing hooked.");
			return 0;
		}
		s_rotate = reinterpret_cast<rotate_fn>(s_base + ROTATE_RVA);
		s_multiply = reinterpret_cast<multiply_fn>(s_base + MULTIPLY_RVA);
		for (int hand = 0; hand < 2; ++hand)
		{
			if (std::memcmp(reinterpret_cast<void *>(s_base + HAND_ROTATION_RVA[hand]), HAND_GETTER_BYTES, sizeof(HAND_GETTER_BYTES)) != 0 ||
				std::memcmp(reinterpret_cast<void *>(s_base + HAND_POSITION_RVA[hand]), HAND_GETTER_BYTES, sizeof(HAND_GETTER_BYTES)) != 0)
			{
				log_line("Game version not recognised (controller getters differ), nothing hooked.");
				return 0;
			}
			s_hand_rotation[hand] = reinterpret_cast<hand_getter_fn>(s_base + HAND_ROTATION_RVA[hand]);
			s_hand_position[hand] = reinterpret_cast<hand_getter_fn>(s_base + HAND_POSITION_RVA[hand]);
		}
		const MH_STATUS init = MH_Initialize();
		if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
			MH_CreateHook(target, reinterpret_cast<void *>(&hooked_update_camera), reinterpret_cast<void **>(&s_original)) != MH_OK ||
			MH_CreateHook(get_transform, reinterpret_cast<void *>(&hooked_get_transform), reinterpret_cast<void **>(&s_original_get_transform)) != MH_OK ||
			MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
		{
			log_line("Could not hook the capture camera update.");
			return 0;
		}
		log_line("EchoCam active (mode %s). Start Echo with -capturevp2; F10 logs the head and hand poses.",
			s_mode == MODE_HEAD ? "head" : s_mode == MODE_HAND ? "hand" : "third_person");
		tablet::start(reinterpret_cast<unsigned char *>(s_base), locked_log);
		panel::start(ini_path(), locked_log);
		if (HANDLE thread = CreateThread(nullptr, 0, key_thread, nullptr, 0, nullptr))
			CloseHandle(thread);
		return 0;
	}
}

namespace bridge
{
	bool world_to_tracking(const float *world_position, const float *world_rotation, float *position, float *rotation)
	{
		uintptr_t renderer = 0, node = 0, set = 0, array = 0;
		uint16_t index = 0;
		float node_pose[8], space[4], offset[3];
		if (!safe_read(s_base + RENDERER_RVA, renderer) || !safe_read(renderer + HEAD_NODE, node) || !node || !safe_read(node + NODE_SET, set) ||
			!safe_read(set + NODE_ARRAY, array) || !safe_read(node + NODE_INDEX, index) ||
			!safe_copy(node_pose, reinterpret_cast<const void *>(array + index * NODE_SIZE), sizeof(node_pose)) ||
			!safe_copy(space, reinterpret_cast<const void *>(s_base + PLAY_SPACE_ROTATION_RVA), sizeof(space)) ||
			!safe_copy(offset, reinterpret_cast<const void *>(s_base + HEAD_OFFSET_RVA), sizeof(offset)))
			return false;
		// world = node.p + node.q * (space * (flip(tracking) + offset)); flip turns x and z round
		const float relative[3] = { world_position[0] - node_pose[4], world_position[1] - node_pose[5], world_position[2] - node_pose[6] };
		float local[3], flipped[3];
		conj_rotate(node_pose, relative, local);
		conj_rotate(space, local, flipped);
		position[0] = -(flipped[0] - offset[0]);
		position[1] = flipped[1] - offset[1];
		position[2] = -(flipped[2] - offset[2]);
		const float node_inverse[4] = { -node_pose[0], -node_pose[1], -node_pose[2], node_pose[3] };
		const float space_inverse[4] = { -space[0], -space[1], -space[2], space[3] };
		float q[4];
		quat_multiply(space_inverse, node_inverse, q);
		quat_multiply(q, world_rotation, q);
		rotation[0] = -q[0];
		rotation[1] = q[1];
		rotation[2] = -q[2];
		rotation[3] = q[3];
		return true;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		GetModuleFileNameW(module, s_dir, MAX_PATH);
		if (wchar_t *slash = wcsrchr(s_dir, L'\\'))
			*slash = L'\0';
		// Hooking from DllMain would run under the loader lock
		if (HANDLE thread = CreateThread(nullptr, 0, install, nullptr, 0, nullptr))
			CloseHandle(thread);
	}
	return TRUE;
}
