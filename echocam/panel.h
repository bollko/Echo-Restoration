// EchoCam side panel: a quad layer the Oculus compositor draws beside the tablet hand (panel.cpp).
#pragma once
#include <string>

namespace panel
{
	using log_fn = void (*)(const char *format, ...);
	void start(const std::wstring &ini_path, log_fn log); // after MH_Initialize
	void reload();                                        // EchoCam.ini changed
	// Where the panel is held: hand (0 left, 1 right), offset and rotation (quaternion x y z w) in that controller's
	// Oculus space, width in metres (height = width * 4 / 3)
	void layout(int &hand, float offset[3], float rotation[4], float &width);
}
