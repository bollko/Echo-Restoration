/*
 * EchoCam side panel: a flat panel the Oculus compositor draws beside the tablet hand (an ovrLayerQuad added to the
 * game's frame), independent of the game's own UI.
 *
 * Hooks in LibOVRRT64_1.dll (the runtime the game loads):
 *   ovr_CreateTextureSwapChainDX  the game's D3D12 command queue (the panel's swap chain must use it)
 *   ovr_EndFrame / ovr_SubmitFrame2  adds the panel layer to the layers the game submits
 * The panel's pixels are copied into our own swap chain on the game's queue every frame it is shown.
 *
 * EchoCam.ini, section [Panel] (re-read while the game runs):
 *   Show=0                1 shows the panel
 *   Hand=left             the hand it is held by
 *   Offset=0.2 0.05 0     metres in the controller's space (x right, y up, z back)
 *   Rotation=0 -90 0      yaw pitch roll in degrees, applied in the controller's space
 *   Width=0.22            metres (the height follows the picture's aspect, 3:4 portrait)
 *   Scale=1.3             size of the panel beside the tablet, as a multiple of the tablet's height
 *   Nudge=0.0175 0.0125   moves it along the tablet's edges: right, up (metres)
 *   TextureState=128      D3D12_RESOURCE_STATES the swap chain textures are in between frames (128 = PIXEL_SHADER_RESOURCE)
 * While the CAMERA tile is open, ArcadeHost shares the panel's picture (panel_ipc.h) and the panel shows by itself;
 * Show=1 shows a test pattern when nothing is shared.
 *
 * Touch: the other hand's index fingertip (Finger, in that controller's space) is tracked against the panel's plane.
 * Within Hover metres it reports where it points (ArcadeHost draws a dot); within Press metres it presses, pulling back
 * past Release metres releases. Presses buzz the finger's controller.
 *   Finger=0 -0.02 -0.08  fingertip in the tapping controller's space (metres)
 *
 * Fit to the tablet (ArcadeHost's FIT button, panel_ipc calibrate*): touch the tablet's top-left, top-right and
 * bottom-left corners with the fingertip and press A at each. The panel is then placed flush with the tablet's right edge,
 * as tall as the tablet, and saved as Offset, RotationQuat (x y z w, overrides Rotation) and Width.
 *   Press=0.012           Release=0.03           Hover=0.12
 */
#include "panel.h"
#include "panel_ipc.h"
#include "tablet.h"
#include "bridge.h"
#include "spatial_ipc.h"
#include <Windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "../discglow/include/MinHook.h"

#pragma comment(lib, "d3d12.lib")

namespace
{
	// ---- LibOVR (OVR_CAPI.h, 64-bit layouts) ----
	using ovrSession = void *;
	using ovrTextureSwapChain = void *;
	using ovrResult = int;
	struct ovrQuatf { float x, y, z, w; };
	struct ovrVector3f { float x, y, z; };
	struct ovrPosef { ovrQuatf Orientation; ovrVector3f Position; };
	struct ovrRecti { int x, y, w, h; };
	struct ovrVector2f { float x, y; };
	struct ovrTextureSwapChainDesc { int Type, Format, ArraySize, Width, Height, MipLevels, SampleCount, StaticImage; unsigned MiscFlags, BindFlags; };
	struct alignas(8) ovrLayerHeader { int Type; unsigned Flags; char Reserved[128]; };
	struct alignas(8) ovrLayerQuad { ovrLayerHeader Header; ovrTextureSwapChain ColorTexture; ovrRecti Viewport; ovrPosef QuadPoseCenter; ovrVector2f QuadSize; };
	static_assert(sizeof(ovrLayerHeader) == 136 && offsetof(ovrLayerQuad, ColorTexture) == 136 && offsetof(ovrLayerQuad, QuadSize) == 188, "ovrLayerQuad layout");
	// ovrTrackingState: HeadPose (ovrPoseStatef, 88 bytes), StatusFlags, HandPoses[2] at 0x60, ...; 0x138 bytes in all
	struct ovrTrackingState { unsigned char bytes[0x138]; };
	constexpr size_t HAND_POSES = 0x60, POSE_STATE_SIZE = 0x58;
	constexpr int ovrTexture_2D = 0, OVR_FORMAT_B8G8R8A8_UNORM_SRGB = 7, ovrLayerType_Quad = 3, ovrLayerFlag_HighQuality = 1;

	using create_chain_fn = ovrResult(__cdecl *)(ovrSession, IUnknown *, const ovrTextureSwapChainDesc *, ovrTextureSwapChain *);
	using end_frame_fn = ovrResult(__cdecl *)(ovrSession, long long, const void *, const ovrLayerHeader *const *, unsigned);
	using chain_index_fn = ovrResult(__cdecl *)(ovrSession, ovrTextureSwapChain, int *);
	using chain_buffer_fn = ovrResult(__cdecl *)(ovrSession, ovrTextureSwapChain, int, IID, void **);
	using commit_fn = ovrResult(__cdecl *)(ovrSession, ovrTextureSwapChain);
	using tracking_fn = ovrTrackingState(__cdecl *)(ovrSession, double, int);
	using display_time_fn = double(__cdecl *)(ovrSession, long long);
	using vibration_fn = ovrResult(__cdecl *)(ovrSession, unsigned, float, float);
	constexpr unsigned ovrControllerType_LTouch = 1, ovrControllerType_RTouch = 2;

	create_chain_fn s_create_chain = nullptr;
	end_frame_fn s_end_frame = nullptr, s_submit_frame2 = nullptr;
	chain_index_fn s_chain_index = nullptr;
	chain_buffer_fn s_chain_buffer = nullptr;
	commit_fn s_commit = nullptr;
	tracking_fn s_tracking = nullptr;
	display_time_fn s_display_time = nullptr;
	vibration_fn s_vibration = nullptr;

	panel::log_fn s_log = nullptr;
	std::wstring s_ini;
	std::mutex s_mutex; // settings and the captured queue

	// Settings
	bool s_show = false;
	int s_hand = 0;
	float s_offset[3] = { 0.2f, 0.05f, 0.0f }, s_rotation[3] = { 0, -90, 0 }, s_width = 0.22f;
	float s_finger[3] = { 0, -0.02f, -0.08f }, s_press = 0.012f, s_release = 0.03f, s_hover = 0.12f;
	float s_rotation_quat[4] = { 0, 0, 0, 1 };
	bool s_have_quat = false;
	D3D12_RESOURCE_STATES s_texture_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	ID3D12CommandQueue *s_queue = nullptr; // the game's (AddRef'd)

	// Panel resources (render thread only)
	constexpr int WIDTH = panel_ipc::WIDTH, HEIGHT = panel_ipc::HEIGHT, RING = 3;
	ovrTextureSwapChain s_chain = nullptr;
	ID3D12Device *s_device = nullptr;
	ID3D12CommandAllocator *s_allocators[RING] = {};
	ID3D12GraphicsCommandList *s_list = nullptr;
	ID3D12Resource *s_uploads[RING] = {};
	uint8_t *s_upload_data[RING] = {};
	ID3D12Fence *s_fence = nullptr;
	HANDLE s_fence_event = nullptr;
	UINT64 s_fence_values[RING] = {}, s_next_fence = 1;
	UINT s_row_pitch = 0;
	int s_slot = 0;
	bool s_failed = false;
	std::vector<uint32_t> s_pixels;
	bool s_committed = false, s_showing_shared = false;
	LONG s_shown_serial = -1;

	// The picture ArcadeHost shares (render thread only)
	panel_ipc::Shared *s_shared = nullptr;
	ULONGLONG s_last_open = 0;

	void open_shared()
	{
		if (s_shared != nullptr || GetTickCount64() - s_last_open < 1000)
			return;
		s_last_open = GetTickCount64();
		if (HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, panel_ipc::NAME))
		{
			s_shared = static_cast<panel_ipc::Shared *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(panel_ipc::Shared)));
			CloseHandle(map); // the view keeps the mapping
			if (s_shared)
				s_log("panel: connected to ArcadeHost's panel picture");
		}
	}

	// ---- settings ----

	void load_settings()
	{
		const wchar_t *ini = s_ini.c_str();
		wchar_t text[96];
		const std::lock_guard<std::mutex> lock(s_mutex);
		s_show = GetPrivateProfileIntW(L"Panel", L"Show", 0, ini) != 0;
		GetPrivateProfileStringW(L"Panel", L"Hand", L"left", text, 96, ini);
		s_hand = _wcsicmp(text, L"right") == 0 ? 1 : 0;
		GetPrivateProfileStringW(L"Panel", L"Offset", L"0.2 0.05 0", text, 96, ini);
		swscanf_s(text, L"%f %f %f", &s_offset[0], &s_offset[1], &s_offset[2]);
		GetPrivateProfileStringW(L"Panel", L"RotationQuat", L"", text, 96, ini);
		s_have_quat = swscanf_s(text, L"%f %f %f %f", &s_rotation_quat[0], &s_rotation_quat[1], &s_rotation_quat[2], &s_rotation_quat[3]) == 4;
		GetPrivateProfileStringW(L"Panel", L"Rotation", L"0 -90 0", text, 96, ini);
		swscanf_s(text, L"%f %f %f", &s_rotation[0], &s_rotation[1], &s_rotation[2]);
		GetPrivateProfileStringW(L"Panel", L"Width", L"0.22", text, 96, ini);
		swscanf_s(text, L"%f", &s_width);
		GetPrivateProfileStringW(L"Panel", L"Finger", L"0 -0.02 -0.08", text, 96, ini);
		swscanf_s(text, L"%f %f %f", &s_finger[0], &s_finger[1], &s_finger[2]);
		GetPrivateProfileStringW(L"Panel", L"Press", L"0.012", text, 96, ini);
		swscanf_s(text, L"%f", &s_press);
		GetPrivateProfileStringW(L"Panel", L"Release", L"0.03", text, 96, ini);
		swscanf_s(text, L"%f", &s_release);
		GetPrivateProfileStringW(L"Panel", L"Hover", L"0.12", text, 96, ini);
		swscanf_s(text, L"%f", &s_hover);
		GetPrivateProfileStringW(L"Panel", L"Scale", L"1.3", text, 96, ini);
		float scale = 1.3f;
		swscanf_s(text, L"%f", &scale);
		tablet::set_scale(scale);
		GetPrivateProfileStringW(L"Panel", L"Nudge", L"0.0175 0.0125", text, 96, ini);
		float nudge[2] = { 0.0175f, 0.0125f };
		swscanf_s(text, L"%f %f", &nudge[0], &nudge[1]);
		tablet::set_nudge(nudge[0], nudge[1]);
		s_texture_state = static_cast<D3D12_RESOURCE_STATES>(GetPrivateProfileIntW(L"Panel", L"TextureState", D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, ini));
	}

	// ---- math ----

	ovrQuatf multiply(const ovrQuatf &a, const ovrQuatf &b)
	{
		return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
	}

	ovrVector3f rotate(const ovrQuatf &q, const ovrVector3f &v)
	{
		const float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
		return { v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx) };
	}

	// Yaw (y), pitch (x), roll (z) in degrees: q = yaw * pitch * roll
	ovrQuatf euler(const float *degrees)
	{
		const float r = 0.0174532925f / 2;
		const ovrQuatf yaw{ 0, std::sin(degrees[0] * r), 0, std::cos(degrees[0] * r) };
		const ovrQuatf pitch{ std::sin(degrees[1] * r), 0, 0, std::cos(degrees[1] * r) };
		const ovrQuatf roll{ 0, 0, std::sin(degrees[2] * r), std::cos(degrees[2] * r) };
		return multiply(multiply(yaw, pitch), roll);
	}

	// The panel's rotation in the holding controller's space (settings lock not needed: render thread copies)
	ovrQuatf held_rotation(const float *degrees)
	{
		bool have;
		ovrQuatf q;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			have = s_have_quat;
			q = { s_rotation_quat[0], s_rotation_quat[1], s_rotation_quat[2], s_rotation_quat[3] };
		}
		return have ? q : euler(degrees);
	}

	// ---- content ----

	void test_pattern()
	{
		s_pixels.resize(WIDTH * HEIGHT);
		for (int y = 0; y < HEIGHT; ++y)
			for (int x = 0; x < WIDTH; ++x)
			{
				uint32_t c = 0xff181c28; // dark panel
				if (x < 8 || y < 8 || x >= WIDTH - 8 || y >= HEIGHT - 8)
					c = 0xff5ac8ff; // frame
				else if (y > HEIGHT / 3 && y < HEIGHT * 2 / 3)
				{
					static const uint32_t bars[6] = { 0xffff4040, 0xffffa020, 0xfff0e040, 0xff40d060, 0xff4090ff, 0xffb060ff };
					c = bars[x * 6 / WIDTH];
				}
				s_pixels[y * WIDTH + x] = c;
			}
	}

	// ---- D3D12 ----

	bool create_resources(ovrSession session, ID3D12CommandQueue *queue)
	{
		if (FAILED(queue->GetDevice(IID_PPV_ARGS(&s_device))))
			return false;
		const ovrTextureSwapChainDesc desc{ ovrTexture_2D, OVR_FORMAT_B8G8R8A8_UNORM_SRGB, 1, WIDTH, HEIGHT, 1, 1, 0, 0, 0 };
		if (s_create_chain(session, queue, &desc, &s_chain) < 0 || s_chain == nullptr)
		{
			s_log("panel: could not create the swap chain");
			return false;
		}
		const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
		for (auto &allocator : s_allocators)
			if (FAILED(s_device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))))
				return false;
		if (FAILED(s_device->CreateCommandList(0, type, s_allocators[0], nullptr, IID_PPV_ARGS(&s_list))))
			return false;
		s_list->Close();
		if (FAILED(s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence))))
			return false;
		s_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

		s_row_pitch = (WIDTH * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = UINT64(s_row_pitch) * HEIGHT;
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		for (int i = 0; i < RING; ++i)
		{
			if (FAILED(s_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s_uploads[i]))))
				return false;
			D3D12_RANGE none{ 0, 0 };
			if (FAILED(s_uploads[i]->Map(0, &none, reinterpret_cast<void **>(&s_upload_data[i]))))
				return false;
		}
		s_log("panel: swap chain and upload buffers ready");
		return true;
	}

	// Copies s_pixels into the swap chain's current texture and commits it
	bool upload(ovrSession session, ID3D12CommandQueue *queue, D3D12_RESOURCE_STATES state)
	{
		int index = 0;
		ID3D12Resource *texture = nullptr;
		if (s_chain_index(session, s_chain, &index) < 0 || s_chain_buffer(session, s_chain, index, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&texture)) < 0 || !texture)
			return false;

		const int slot = s_slot;
		s_slot = (s_slot + 1) % RING;
		if (s_fence->GetCompletedValue() < s_fence_values[slot])
		{
			s_fence->SetEventOnCompletion(s_fence_values[slot], s_fence_event);
			WaitForSingleObject(s_fence_event, 100);
		}
		for (int y = 0; y < HEIGHT; ++y)
			std::memcpy(s_upload_data[slot] + size_t(y) * s_row_pitch, &s_pixels[size_t(y) * WIDTH], WIDTH * 4);

		s_allocators[slot]->Reset();
		s_list->Reset(s_allocators[slot], nullptr);
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = state;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		if (state != D3D12_RESOURCE_STATE_COPY_DEST)
			s_list->ResourceBarrier(1, &barrier);

		D3D12_TEXTURE_COPY_LOCATION dst{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		dst.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION src{ s_uploads[slot], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
		src.PlacedFootprint.Footprint = { texture->GetDesc().Format, UINT(WIDTH), UINT(HEIGHT), 1, s_row_pitch };
		s_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		if (state != D3D12_RESOURCE_STATE_COPY_DEST)
			s_list->ResourceBarrier(1, &barrier);
		s_list->Close();
		ID3D12CommandList *lists[] = { s_list };
		queue->ExecuteCommandLists(1, lists);
		s_fence_values[slot] = s_next_fence++;
		queue->Signal(s_fence, s_fence_values[slot]);
		texture->Release();
		return s_commit(session, s_chain) >= 0;
	}

	// ---- touch ----

	bool s_touching = false;
	float s_touch_x = 0, s_touch_y = 0;
	int s_buzz_frames = 0;

	void send_touch(bool down, float x, float y)
	{
		const LONG index = s_shared->touchWrite;
		panel_ipc::Touch &t = s_shared->touches[index % panel_ipc::TOUCHES];
		t.down = down;
		t.x = x;
		t.y = y;
		MemoryBarrier();
		t.seq = index + 1;
		s_shared->touchWrite = index + 1;
	}

	// Follows the tapping hand's fingertip against the panel (pose, size) and reports hovers and presses
	void track_finger(ovrSession session, const ovrTrackingState &tracking, int finger_hand, const ovrPosef &panel_pose, ovrVector2f size,
		const float *finger, float press, float release, float hover)
	{
		if (s_buzz_frames > 0 && --s_buzz_frames == 0 && s_vibration)
			s_vibration(session, finger_hand ? ovrControllerType_RTouch : ovrControllerType_LTouch, 0, 0);
		if (s_shared == nullptr)
			return;
		ovrPosef pose;
		std::memcpy(&pose, tracking.bytes + HAND_POSES + finger_hand * POSE_STATE_SIZE, sizeof(pose));
		const ovrVector3f tip_offset = rotate(pose.Orientation, { finger[0], finger[1], finger[2] });
		const ovrVector3f relative{ pose.Position.x + tip_offset.x - panel_pose.Position.x, pose.Position.y + tip_offset.y - panel_pose.Position.y,
			pose.Position.z + tip_offset.z - panel_pose.Position.z };
		const ovrQuatf inverse{ -panel_pose.Orientation.x, -panel_pose.Orientation.y, -panel_pose.Orientation.z, panel_pose.Orientation.w };
		const ovrVector3f local = rotate(inverse, relative); // x right, y up, z out of the panel
		const float u = local.x / size.x + 0.5f, v = 0.5f - local.y / size.y;
		const float x = u * WIDTH, y = v * HEIGHT, depth = std::fabs(local.z);
		const bool inside = u >= 0 && u <= 1 && v >= 0 && v <= 1;

		s_shared->hoverX = x;
		s_shared->hoverY = y;
		s_shared->hover = inside && depth < hover;
		if (!s_touching && inside && depth < press)
		{
			s_touching = true;
			s_touch_x = x;
			s_touch_y = y;
			send_touch(true, x, y);
			if (s_vibration && s_vibration(session, finger_hand ? ovrControllerType_RTouch : ovrControllerType_LTouch, 1.0f, 0.4f) >= 0)
				s_buzz_frames = 4;
		}
		else if (s_touching && (depth > release || u < -0.05f || u > 1.05f || v < -0.05f || v > 1.05f))
		{
			s_touching = false;
			send_touch(false, s_touch_x, s_touch_y);
		}
	}

	// ---- fitting the panel to the tablet ----

	using input_fn = ovrResult(__cdecl *)(ovrSession, unsigned, void *);
	input_fn s_input = nullptr;
	constexpr unsigned ovrControllerType_Touch = 3, ovrButton_A = 1;
	LONG s_calibrate_seen = 0;
	int s_calibrate_step = 0; // 1..3: waiting for that corner
	ovrVector3f s_corners[3];
	bool s_a_was_down = true;

	ovrVector3f sub(const ovrVector3f &a, const ovrVector3f &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	ovrVector3f scale(const ovrVector3f &a, float k) { return { a.x * k, a.y * k, a.z * k }; }
	float dot(const ovrVector3f &a, const ovrVector3f &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	ovrVector3f cross(const ovrVector3f &a, const ovrVector3f &b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	ovrVector3f normal(const ovrVector3f &a) { const float l = std::sqrt(dot(a, a)); return l > 1e-6f ? scale(a, 1 / l) : a; }

	// Rotation whose x, y, z axes are the given (orthonormal, right-handed) columns
	ovrQuatf from_axes(const ovrVector3f &x, const ovrVector3f &y, const ovrVector3f &z)
	{
		const float trace = x.x + y.y + z.z;
		ovrQuatf q;
		if (trace > 0)
		{
			const float s = std::sqrt(trace + 1) * 2;
			q = { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4 };
		}
		else if (x.x > y.y && x.x > z.z)
		{
			const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
			q = { s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s };
		}
		else if (y.y > z.z)
		{
			const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
			q = { (y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s };
		}
		else
		{
			const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
			q = { (z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s };
		}
		return q;
	}

	ovrVector3f to_local(const ovrPosef &frame, const ovrVector3f &world)
	{
		const ovrQuatf inverse{ -frame.Orientation.x, -frame.Orientation.y, -frame.Orientation.z, frame.Orientation.w };
		return rotate(inverse, sub(world, frame.Position));
	}

	// From the three corners (in the holding controller's space) and the head: places the panel and saves it
	void save_fit(const ovrVector3f &head)
	{
		const ovrVector3f top_left = s_corners[0], across = sub(s_corners[1], top_left), down_edge = sub(s_corners[2], top_left);
		ovrVector3f x = normal(across);
		ovrVector3f y = normal(sub(scale(x, dot(down_edge, x)), down_edge)); // up, square to x
		ovrVector3f z = cross(x, y);                                        // out of the tablet
		if (dot(z, sub(head, top_left)) < 0) // corners touched mirrored: face the head anyway
		{
			x = scale(x, -1);
			z = scale(z, -1);
		}
		const float width = std::sqrt(dot(across, across)), height = std::fabs(dot(down_edge, y));
		const float panel_width = height * 3 / 4, gap = 0.01f;
		const ovrVector3f centre = sub(scale(x, dot(across, x) + gap + panel_width / 2), scale(y, height / 2));
		const ovrVector3f offset{ top_left.x + centre.x, top_left.y + centre.y, top_left.z + centre.z };
		const ovrQuatf q = from_axes(x, y, z);
		wchar_t text[128];
		swprintf_s(text, L"%.4f %.4f %.4f", offset.x, offset.y, offset.z);
		WritePrivateProfileStringW(L"Panel", L"Offset", text, s_ini.c_str());
		swprintf_s(text, L"%.5f %.5f %.5f %.5f", q.x, q.y, q.z, q.w);
		WritePrivateProfileStringW(L"Panel", L"RotationQuat", text, s_ini.c_str());
		swprintf_s(text, L"%.4f", panel_width);
		WritePrivateProfileStringW(L"Panel", L"Width", text, s_ini.c_str());
		load_settings();
		s_log("panel: fitted to the tablet (%.3f x %.3f m): offset %.3f %.3f %.3f, width %.3f", width, height, offset.x, offset.y, offset.z, panel_width);
	}

	// Waits for A presses with the fingertip on each corner (render thread)
	void calibrate(ovrSession session, const ovrTrackingState &tracking, int hand, const float *finger)
	{
		if (s_shared == nullptr || s_input == nullptr)
			return;
		if (s_shared->calibrateRequest != s_calibrate_seen)
		{
			s_calibrate_seen = s_shared->calibrateRequest;
			s_calibrate_step = 1;
			s_a_was_down = true; // the press that started it does not count
		}
		s_shared->calibrateStep = s_calibrate_step;
		if (s_calibrate_step == 0)
			return;
		unsigned char input[256] = {};
		const bool a_down = s_input(session, ovrControllerType_Touch, input) >= 0 && (*reinterpret_cast<unsigned *>(input + 8) & ovrButton_A) != 0;
		const bool pressed = a_down && !s_a_was_down;
		s_a_was_down = a_down;
		if (!pressed)
			return;
		const int finger_hand = 1 - hand;
		ovrPosef holder, tapper, head;
		std::memcpy(&holder, tracking.bytes + HAND_POSES + hand * POSE_STATE_SIZE, sizeof(holder));
		std::memcpy(&tapper, tracking.bytes + HAND_POSES + finger_hand * POSE_STATE_SIZE, sizeof(tapper));
		std::memcpy(&head, tracking.bytes, sizeof(head));
		const ovrVector3f tip_offset = rotate(tapper.Orientation, { finger[0], finger[1], finger[2] });
		const ovrVector3f tip{ tapper.Position.x + tip_offset.x, tapper.Position.y + tip_offset.y, tapper.Position.z + tip_offset.z };
		s_corners[s_calibrate_step - 1] = to_local(holder, tip);
		if (s_vibration && s_vibration(session, finger_hand ? ovrControllerType_RTouch : ovrControllerType_LTouch, 1.0f, 0.6f) >= 0)
			s_buzz_frames = 8;
		if (++s_calibrate_step > 3)
		{
			s_calibrate_step = 0;
			save_fit(to_local(holder, head.Position));
			InterlockedIncrement(&s_shared->calibrateDone);
		}
		s_shared->calibrateStep = s_calibrate_step;
	}

	// ---- the tablet's place relative to the head, for ArcadeHost's spatial sound (spatial_ipc.h) ----

	spatial_ipc::Shared *s_spatial = nullptr;

	void publish_tablet(ovrSession session, long long frame)
	{
		if (s_spatial == nullptr)
		{
			static bool tried = false;
			if (tried)
				return;
			tried = true;
			if (HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(spatial_ipc::Shared), spatial_ipc::NAME))
				s_spatial = static_cast<spatial_ipc::Shared *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(spatial_ipc::Shared)));
			if (s_spatial == nullptr)
				return;
			s_spatial->magic = spatial_ipc::MAGIC;
		}
		const ovrTrackingState tracking = s_tracking(session, s_display_time(session, frame), 0);
		ovrPosef head;
		std::memcpy(&head, tracking.bytes, sizeof(head));
		const float identity[4] = { 0, 0, 0, 1 };
		float tracking_p[3], tracking_q[4];
		LONG kind = spatial_ipc::Tablet;
		// Docked on lobby posters (EchoArcade): the nearest of the posters showing the arcade, else the tablet's screen
		float sources[spatial_ipc::MAX_SOURCES][3];
		LONG count = 0, before = s_spatial->sourceSeq;
		if (!(before & 1) && static_cast<LONGLONG>(GetTickCount64()) - s_spatial->sourceTime < 1000)
		{
			count = (std::min)(static_cast<LONG>(spatial_ipc::MAX_SOURCES), static_cast<LONG>(s_spatial->sourceCount));
			std::memcpy(sources, const_cast<const float(*)[3]>(s_spatial->sources), sizeof(float) * 3 * (count > 0 ? count : 0));
			if (s_spatial->sourceSeq != before)
				count = 0;
		}
		float best = 1e9f;
		for (LONG i = 0; i < count; ++i)
		{
			float candidate[3], q[4];
			if (!bridge::world_to_tracking(sources[i], identity, candidate, q))
				continue;
			const float dx = candidate[0] - head.Position.x, dy = candidate[1] - head.Position.y, dz = candidate[2] - head.Position.z;
			const float d = dx * dx + dy * dy + dz * dz;
			if (d < best)
			{
				best = d;
				std::memcpy(tracking_p, candidate, sizeof(tracking_p));
				kind = spatial_ipc::Poster;
			}
		}
		if (kind == spatial_ipc::Tablet)
		{
			tablet::PanelPose p;
			if (!tablet::panel_pose(p) || !bridge::world_to_tracking(p.screen, identity, tracking_p, tracking_q))
				return;
		}
		const ovrQuatf inverse{ -head.Orientation.x, -head.Orientation.y, -head.Orientation.z, head.Orientation.w };
		const ovrVector3f local = rotate(inverse, { tracking_p[0] - head.Position.x, tracking_p[1] - head.Position.y, tracking_p[2] - head.Position.z });
		InterlockedIncrement(&s_spatial->seq); // odd: being written
		s_spatial->position[0] = local.x;
		s_spatial->position[1] = local.y;
		s_spatial->position[2] = local.z;
		s_spatial->kind = kind;
		s_spatial->time = static_cast<LONGLONG>(GetTickCount64());
		InterlockedIncrement(&s_spatial->seq);
	}

	// ---- frame hook ----

	ovrResult submit(end_frame_fn original, ovrSession session, long long frame, const void *view_scale, const ovrLayerHeader *const *layers, unsigned count)
	{
		bool show;
		int hand;
		float offset[3], rotation[3], width, finger[3], press, release, hover;
		D3D12_RESOURCE_STATES state;
		ID3D12CommandQueue *queue;
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			show = s_show;
			hand = s_hand;
			std::memcpy(offset, s_offset, sizeof(offset));
			std::memcpy(rotation, s_rotation, sizeof(rotation));
			width = s_width;
			std::memcpy(finger, s_finger, sizeof(finger));
			press = s_press;
			release = s_release;
			hover = s_hover;
			state = s_texture_state;
			queue = s_queue;
		}
		open_shared();
		if (s_tracking && s_display_time && layers != nullptr)
			publish_tablet(session, frame);
		const bool shared = s_shared != nullptr && s_shared->magic == panel_ipc::MAGIC && s_shared->visible != 0;
		if ((!show && !shared) || s_failed || queue == nullptr || layers == nullptr || count >= 16)
			return original(session, frame, view_scale, layers, count);

		if (s_chain == nullptr && !create_resources(session, queue))
		{
			s_failed = true;
			s_log("panel: setup failed; the panel stays off");
			return original(session, frame, view_scale, layers, count);
		}
		// New picture: upload it (the compositor keeps showing the last committed one meanwhile)
		const LONG serial = shared ? s_shared->serial : 0;
		if (!s_committed || shared != s_showing_shared || (shared && serial != s_shown_serial))
		{
			if (shared)
			{
				s_pixels.resize(size_t(WIDTH) * HEIGHT);
				std::memcpy(s_pixels.data(), s_shared->frames[s_shared->front & 1], s_pixels.size() * 4);
			}
			else
				test_pattern();
			if (upload(session, queue, state))
			{
				s_committed = true;
				s_showing_shared = shared;
				s_shown_serial = serial;
			}
		}
		if (!s_committed)
			return original(session, frame, view_scale, layers, count);

		// The hand's pose in tracking space (the same space as the game's eye layer)
		const ovrTrackingState tracking = s_tracking(session, s_display_time(session, frame), 0);
		ovrPosef hand_pose;
		std::memcpy(&hand_pose, tracking.bytes + HAND_POSES + hand * POSE_STATE_SIZE, sizeof(hand_pose));
		const ovrVector3f moved = rotate(hand_pose.Orientation, { offset[0], offset[1], offset[2] });

		static ovrLayerQuad quad;
		quad = {};
		quad.Header.Type = ovrLayerType_Quad;
		quad.Header.Flags = ovrLayerFlag_HighQuality;
		quad.ColorTexture = s_chain;
		quad.Viewport = { 0, 0, WIDTH, HEIGHT };
		quad.QuadPoseCenter.Orientation = multiply(hand_pose.Orientation, held_rotation(rotation));
		quad.QuadPoseCenter.Position = { hand_pose.Position.x + moved.x, hand_pose.Position.y + moved.y, hand_pose.Position.z + moved.z };
		quad.QuadSize = { width, width * HEIGHT / WIDTH };
		// Flush with the tablet itself when its buttons can be seen (tablet.cpp), else held as configured above
		tablet::PanelPose fitted;
		float world_q[4], tracking_p[3], tracking_q[4];
		if (tablet::panel_pose(fitted))
		{
			// The compositor shows a quad's front along its -Z: turn it half round about up so the picture is not mirrored
			const float left[3] = { -fitted.right[0], -fitted.right[1], -fitted.right[2] };
			const float in[3] = { -fitted.out[0], -fitted.out[1], -fitted.out[2] };
			bridge::quat_from_axes(left, fitted.up, in, world_q);
			if (bridge::world_to_tracking(fitted.centre, world_q, tracking_p, tracking_q))
			{
				quad.QuadPoseCenter.Orientation = { tracking_q[0], tracking_q[1], tracking_q[2], tracking_q[3] };
				quad.QuadPoseCenter.Position = { tracking_p[0], tracking_p[1], tracking_p[2] };
				quad.QuadSize = { fitted.width, fitted.height };
			}
		}
		if (shared)
		{
			calibrate(session, tracking, hand, finger);
			if (s_calibrate_step == 0) // touching the tablet's corners must not press panel controls
				track_finger(session, tracking, 1 - hand, quad.QuadPoseCenter, quad.QuadSize, finger, press, release, hover);
			else
				s_shared->hover = 0;
		}

		const ovrLayerHeader *list[17];
		for (unsigned i = 0; i < count; ++i)
			list[i] = layers[i];
		list[count] = &quad.Header;
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			s_log("panel: first frame with the panel layer (game layers %u)", count);
		}
		return original(session, frame, view_scale, list, count + 1);
	}

	ovrResult __cdecl hooked_end_frame(ovrSession session, long long frame, const void *view_scale, const ovrLayerHeader *const *layers, unsigned count)
	{
		return submit(s_end_frame, session, frame, view_scale, layers, count);
	}

	ovrResult __cdecl hooked_submit_frame2(ovrSession session, long long frame, const void *view_scale, const ovrLayerHeader *const *layers, unsigned count)
	{
		return submit(s_submit_frame2, session, frame, view_scale, layers, count);
	}

	ovrResult __cdecl hooked_create_chain(ovrSession session, IUnknown *device, const ovrTextureSwapChainDesc *desc, ovrTextureSwapChain *out)
	{
		ID3D12CommandQueue *queue = nullptr;
		if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			if (s_queue == nullptr)
			{
				s_queue = queue; // keeps the reference
				queue = nullptr;
				s_log("panel: the game's D3D12 queue is %p", s_queue);
			}
		}
		if (queue)
			queue->Release();
		return s_create_chain(session, device, desc, out);
	}

	template <typename T>
	bool hook(HMODULE module, const char *name, void *detour, T *original)
	{
		void *target = reinterpret_cast<void *>(GetProcAddress(module, name));
		return target != nullptr && MH_CreateHook(target, detour, reinterpret_cast<void **>(original)) == MH_OK && MH_EnableHook(target) == MH_OK;
	}

	DWORD WINAPI install(void *)
	{
		// The game loads the Oculus runtime shortly after start, before it creates its swap chains
		HMODULE ovr = nullptr;
		for (int i = 0; i < 60000 && !ovr; ++i)
		{
			ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
			if (!ovr)
				Sleep(1);
		}
		if (!ovr)
		{
			s_log("panel: Oculus runtime (LibOVRRT64_1.dll) not found");
			return 0;
		}
		s_chain_index = reinterpret_cast<chain_index_fn>(GetProcAddress(ovr, "ovr_GetTextureSwapChainCurrentIndex"));
		s_chain_buffer = reinterpret_cast<chain_buffer_fn>(GetProcAddress(ovr, "ovr_GetTextureSwapChainBufferDX"));
		s_commit = reinterpret_cast<commit_fn>(GetProcAddress(ovr, "ovr_CommitTextureSwapChain"));
		s_tracking = reinterpret_cast<tracking_fn>(GetProcAddress(ovr, "ovr_GetTrackingState"));
		s_display_time = reinterpret_cast<display_time_fn>(GetProcAddress(ovr, "ovr_GetPredictedDisplayTime"));
		s_vibration = reinterpret_cast<vibration_fn>(GetProcAddress(ovr, "ovr_SetControllerVibration"));
		s_input = reinterpret_cast<input_fn>(GetProcAddress(ovr, "ovr_GetInputState"));
		const bool chain = hook(ovr, "ovr_CreateTextureSwapChainDX", reinterpret_cast<void *>(&hooked_create_chain), &s_create_chain);
		const bool end = hook(ovr, "ovr_EndFrame", reinterpret_cast<void *>(&hooked_end_frame), &s_end_frame);
		const bool submit2 = hook(ovr, "ovr_SubmitFrame2", reinterpret_cast<void *>(&hooked_submit_frame2), &s_submit_frame2);
		s_log("panel: hooks %s (swap chain %d, EndFrame %d, SubmitFrame2 %d)", chain && (end || submit2) &&
			s_chain_index && s_chain_buffer && s_commit && s_tracking && s_display_time ? "ready" : "INCOMPLETE", chain, end, submit2);
		return 0;
	}
}

namespace panel
{
	void start(const std::wstring &ini_path, log_fn log)
	{
		s_ini = ini_path;
		s_log = log;
		load_settings();
		if (HANDLE thread = CreateThread(nullptr, 0, install, nullptr, 0, nullptr))
			CloseHandle(thread);
	}

	void reload()
	{
		load_settings();
	}

	void layout(int &hand, float offset[3], float rotation[4], float &width)
	{
		float degrees[3];
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			hand = s_hand;
			std::memcpy(offset, s_offset, sizeof(s_offset));
			std::memcpy(degrees, s_rotation, sizeof(degrees));
			width = s_width;
		}
		const ovrQuatf q = held_rotation(degrees);
		rotation[0] = q.x;
		rotation[1] = q.y;
		rotation[2] = q.z;
		rotation[3] = q.w;
	}
}
