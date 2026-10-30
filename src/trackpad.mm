#include "trackpad.h"

#import <Cocoa/Cocoa.h>

namespace {

TrackpadGestures g_pending;
bool g_lastScrollPrecise = false;
id g_monitor = nil;

}

void installTrackpadMonitor()
{
	if (g_monitor != nil) {
		return;
	}
	//a local monitor runs on the main thread before the event reaches GLFW's view, and returning the event passes it on
	g_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskScrollWheel | NSEventMaskMagnify) handler:^NSEvent*(NSEvent* event) {
		if (event.type == NSEventTypeMagnify) {
			g_pending.pinch += (float)event.magnification;
			return event;
		}
		g_lastScrollPrecise = event.hasPreciseScrollingDeltas;
		if (!event.hasPreciseScrollingDeltas || event.momentumPhase != NSEventPhaseNone) {
			return event;
		}
		//a positive delta scrolls toward the top or the left, which moves the content down or right
		const glm::vec2 delta((float)event.scrollingDeltaX, (float)event.scrollingDeltaY);
		const NSEventModifierFlags flags = event.modifierFlags;
		if (flags & NSEventModifierFlagControl) {
			g_pending.controlSwipe += delta;
		} else if (flags & NSEventModifierFlagShift) {
			g_pending.shiftSwipe += delta;
		} else {
			g_pending.swipe += delta;
		}
		return event;
	}];
}

TrackpadGestures takeTrackpadGestures()
{
	const TrackpadGestures taken = g_pending;
	g_pending = {};
	return taken;
}

bool scrollIsFromTrackpad()
{
	return g_lastScrollPrecise;
}
