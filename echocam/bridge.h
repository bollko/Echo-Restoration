// Small helpers shared by EchoCam's parts.
#pragma once
#include <cmath>

namespace bridge
{
	// Game world -> Oculus tracking space, the inverse of how the game places tracked poses (echocam.cpp). Quaternions are
	// x y z w. False until the game has a capture camera node.
	bool world_to_tracking(const float *world_position, const float *world_rotation, float *position, float *rotation);

	// Rotation whose x, y, z axes are the given orthonormal, right-handed vectors
	inline void quat_from_axes(const float *x, const float *y, const float *z, float *q)
	{
		const float trace = x[0] + y[1] + z[2];
		if (trace > 0)
		{
			const float s = std::sqrt(trace + 1) * 2;
			q[0] = (y[2] - z[1]) / s; q[1] = (z[0] - x[2]) / s; q[2] = (x[1] - y[0]) / s; q[3] = s / 4;
		}
		else if (x[0] > y[1] && x[0] > z[2])
		{
			const float s = std::sqrt(1 + x[0] - y[1] - z[2]) * 2;
			q[0] = s / 4; q[1] = (y[0] + x[1]) / s; q[2] = (z[0] + x[2]) / s; q[3] = (y[2] - z[1]) / s;
		}
		else if (y[1] > z[2])
		{
			const float s = std::sqrt(1 + y[1] - x[0] - z[2]) * 2;
			q[0] = (y[0] + x[1]) / s; q[1] = s / 4; q[2] = (z[1] + y[2]) / s; q[3] = (z[0] - x[2]) / s;
		}
		else
		{
			const float s = std::sqrt(1 + z[2] - x[0] - y[1]) * 2;
			q[0] = (z[0] + x[2]) / s; q[1] = (z[1] + y[2]) / s; q[2] = s / 4; q[3] = (x[1] - y[0]) / s;
		}
	}
}
