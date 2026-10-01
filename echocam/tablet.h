// Where the social tablet really is (tablet.cpp): fitted every frame from the world positions of its touch buttons.
#pragma once

namespace tablet
{
	// In game world space: the side panel's centre, its axes (right, up, out towards the viewer) and size in metres,
	// flush with the tablet's right edge. False when the tablet has not been seen for a moment.
	struct PanelPose
	{
		float centre[3], right[3], up[3], out[3];
		float width, height;
	};
	bool panel_pose(PanelPose &pose);
	void set_head(const float *world_position); // the player's head (camera thread): the panel faces it
	void set_scale(float scale);                // panel size multiplier (EchoCam.ini [Panel] Scale)
	void set_nudge(float right, float up);      // metres along the tablet's edges (EchoCam.ini [Panel] Nudge)
	bool start(unsigned char *exe, void (*log)(const char *format, ...)); // after MH_Initialize
}
