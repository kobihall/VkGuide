#include "trackpad.h"

// no gestures outside macOS: every scroll is a mouse wheel's

void installTrackpadMonitor()
{
}

TrackpadGestures takeTrackpadGestures()
{
	return {};
}

bool scrollIsFromTrackpad()
{
	return false;
}
