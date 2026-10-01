// Side panel shared between ArcadeHost and EchoCam.dll: ArcadeHost writes the picture, EchoCam writes the finger
// (hover and touches, in panel pixels). Keep identical to echo-arcade\host\panel_ipc.h.
#pragma once
#include <Windows.h>
#include <cstdint>

namespace panel_ipc
{
	constexpr wchar_t NAME[] = L"Local\\EchoCam.Panel3";
	constexpr uint32_t MAGIC = 0x334C4E50; // "PNL3"
	constexpr int WIDTH = 576, HEIGHT = 768; // portrait 3:4, like a tablet held upright
	constexpr int TOUCHES = 16;

	struct Touch
	{
		volatile LONG seq; // index + 1 once written
		LONG down;         // 1 press, 0 release
		float x, y;        // panel pixels
	};

	struct Shared
	{
		uint32_t magic;
		// ArcadeHost
		volatile LONG visible;  // 1 while the panel should be shown
		volatile LONG serial;   // bumped after each new picture
		volatile LONG front;    // which frame holds the newest picture
		// EchoCam
		volatile LONG hover;    // 1 while a finger is near the panel
		float hoverX, hoverY;   // where it points, panel pixels
		volatile LONG touchWrite;
		Touch touches[TOUCHES];
		// Fitting the panel to the tablet: ArcadeHost bumps calibrateRequest to start; EchoCam reports the corner it waits for
		// in calibrateStep (1 top-left, 2 top-right, 3 bottom-left, 0 idle) and bumps calibrateDone when it saved a fit
		volatile LONG calibrateRequest, calibrateStep, calibrateDone;
		// ArcadeHost
		uint32_t frames[2][WIDTH * HEIGHT]; // BGRA, sRGB; alpha 0 = see-through
	};
}
