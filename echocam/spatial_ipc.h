// Where the browser tiles' sound should come from, relative to the player's head. Shared in the game by EchoArcade.dll
// (the docked lobby posters, game world) and EchoCam.dll (the tablet; it picks the nearest source and writes 'position'),
// read by ArcadeHost, which places the sound there. Keep identical copies in EchoCam, echo-arcade\host and
// echo-arcade\native\runtime.
#pragma once
#include <Windows.h>
#include <cstdint>

namespace spatial_ipc
{
	constexpr wchar_t NAME[] = L"Local\\EchoCam.Spatial2";
	constexpr uint32_t MAGIC = 0x32504153; // "SAP2"
	constexpr int MAX_SOURCES = 16;
	enum Kind { Tablet = 0, Poster = 1 };

	struct Shared
	{
		uint32_t magic;
		// EchoCam: the sound's place
		volatile LONG seq;        // odd while being written
		float position[3];        // metres in the head's frame: x right, y up, z back (Oculus axes)
		volatile LONG kind;       // Kind: a poster is metres away, so it fades more gently
		volatile LONGLONG time;   // GetTickCount64 of the last update
		// EchoArcade: the docked posters (every poster showing the arcade), game world
		volatile LONG sourceSeq;  // odd while being written
		volatile LONG sourceCount;
		float sources[MAX_SOURCES][3];
		volatile LONGLONG sourceTime;
	};
}
