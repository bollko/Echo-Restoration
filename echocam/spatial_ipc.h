// Where the tablet's screen is relative to the player's head, shared by EchoCam.dll (writer, every frame) with ArcadeHost
// (reader), which places the browser tiles' sound there. Keep identical to echo-arcade\host\spatial_ipc.h.
#pragma once
#include <Windows.h>
#include <cstdint>

namespace spatial_ipc
{
	constexpr wchar_t NAME[] = L"Local\\EchoCam.Spatial";
	constexpr uint32_t MAGIC = 0x31504153; // "SAP1"

	struct Shared
	{
		uint32_t magic;
		volatile LONG seq;        // odd while being written
		float position[3];        // metres in the head's frame: x right, y up, z back (Oculus axes)
		volatile LONGLONG time;   // GetTickCount64 of the last update
	};
}
