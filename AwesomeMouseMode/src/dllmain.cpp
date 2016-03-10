#include "stdafx.h"
#include "vstsdk2.4/pluginterfaces/vst2.x/aeffectx.h"

namespace {

// Constants affecting plugin behaviour.
// Constants instead of defines so these can be turned into runtime options.
constexpr bool kEnableDigitizerFix = true;
constexpr bool kEnableTouchPanGesture = true;
constexpr bool kEnableTrackpadZoom = true;
constexpr bool kEnableMultiTouch = false; // Not working.

constexpr bool kEnableScrollingOverride = true;
constexpr int kWheelXScaleNumerator = 2;
constexpr int kWheelXScaleDenominator = 1;
constexpr int kWheelYScaleNumerator = 4;
constexpr int kWheelYScaleDenominator = 3;
constexpr int kMinWheelDelta = 120; // Happens to be equal to WHEEL_DELTA, probably not by accident.

constexpr bool kEnableCtrlShiftZ = true;
constexpr bool kEnablePlusKeyWithoutShift = true;

// Global state, global 'cause we're cool.
struct WindowData {
	HWND hwnd;
	LONG_PTR oldWndProc;
};

// Map of window state, 'cause we don't want to override GWLP_USERDATA, which Live likely already
// uses.
std::recursive_mutex* gStateLock = nullptr;
std::unordered_map<HWND, WindowData>* gWindowMap = nullptr;

bool gDragDown = false;
bool gDragPenDown = false;
POINT gDragDownPoint = { 0, };
POINT gDragPrevDigitizerPoint = { 0, };
POINT gDragDigitizerDelta = { 0, };
bool gHadDigitizerEvent = false;
bool gWasDigitizerClick = false;
bool gHadDigitizerClick = false;
int gHadDoubleClick = 0;
POINT gWheelDeltaAcc = { 0, };
int gWheelZoomDeltaAcc = 0;
bool gInPanGesture = false;
bool gPanGestureDown = false;
bool gPanGestureInInertia = false;
POINT gPanGestureLastPos = { 0, };
POINT gPanGestureBestRealPos = { 0, };
bool gInTrackpadScroll = false;


INPUT MakeKeyboardEvent(int vk, bool keyUp);
LRESULT CALLBACK OurWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void MapWindow(HWND hwnd);
void UnmapWindow(HWND hwnd);
void UnmapAllWindows();
void CALLBACK OurWinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);


// Good ol' Windows stuff.
#define MI_WP_SIGNATURE 0xFF515700
#define SIGNATURE_MASK 0xFFFFFF00
#define IS_PEN_EVENT(dw) (((dw) & SIGNATURE_MASK) == MI_WP_SIGNATURE)

#if defined(_DEBUG)
static void DEBUG_TRACE(LPTSTR format ...) {
	va_list varargs;
	va_start(varargs, format);
	TCHAR outstr[1024];
	_vstprintf_s(outstr, format, varargs);
	OutputDebugString(outstr);
}
#else
#define DEBUG_TRACE(...)
#endif


// Handle events forwarded from windows we're hooked into. This lets us override what Live sees and
// change behaviour.
LRESULT CALLBACK OurWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	WindowData data;
	{
		std::lock_guard<std::recursive_mutex> lock(*gStateLock);
		auto findIt = gWindowMap->find(hwnd);
		if (findIt == gWindowMap->end()) {
			return 0L;
		}
		data = findIt->second;
	}

	WORD loword = LOWORD(wParam);
	switch (msg) {
	case WM_LBUTTONDOWN:
		if (kEnableDigitizerFix) {
			POINT mpos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			bool isPenEvent = IS_PEN_EVENT(::GetMessageExtraInfo());
			if (isPenEvent) {
				DEBUG_TRACE(_T("BUTTONDOWN: [%d, %d]\n"), mpos.x, mpos.y);
				gHadDigitizerClick = true;
				gHadDigitizerEvent = true;
				gDragPenDown = true;
			} else {
				gDragPenDown = false;
			}
			gDragDown = true;
			gDragDownPoint = mpos;
			gWasDigitizerClick = false;
			gDragDigitizerDelta.x = 0;
			gDragDigitizerDelta.y = 0;
		}
		break;
	case WM_LBUTTONUP:
		gDragDown = false;
		gDragPenDown = false;
		break;
	case WM_LBUTTONDBLCLK:
		if (kEnableDigitizerFix) {
			bool isPenEvent = IS_PEN_EVENT(::GetMessageExtraInfo());
			if (isPenEvent) {
				// _TODO_ Initial fix for jumping around when Windows thinks we double clicked (but still broken).
				DEBUG_TRACE(_T("DBLCLK Reset: [%d, %d]\n"), gDragPrevDigitizerPoint.x, gDragPrevDigitizerPoint.y);
				lParam = MAKELONG((short) gDragPrevDigitizerPoint.x, (short) gDragPrevDigitizerPoint.y);
				gHadDoubleClick = 3;
			}
		}
		break;
	case WM_MOUSEMOVE:
		if (kEnableTouchPanGesture) {
			if (!gInPanGesture && gPanGestureDown) {
				// Cancel pan because the mouse is moving.
				gPanGestureDown = false;
				DEBUG_TRACE(_T("MOUSEMOVE: Force Pan Gesture Up\n"));
				::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, WM_LBUTTONUP, 0, MAKELONG(gPanGestureLastPos.x, gPanGestureLastPos.y));
				::SetCursorPos(gPanGestureBestRealPos.x, gPanGestureBestRealPos.y);
				return 0L;
			} else if (gPanGestureDown && !gPanGestureInInertia && !(gPanGestureLastPos.x == GET_X_LPARAM(lParam) && gPanGestureLastPos.y == GET_Y_LPARAM(lParam))) {
				// The mouse moved while panning. Ignore it.
				return 0L;
			} else {
				// We get one WM_MOUSEMOVE event just after the gesture begins. We need to treat it as a
				// non-mouse event, and skip canceling the pan.
				gInPanGesture = false;
				if (!gPanGestureDown) {
					POINT realScreenPoint;
					::GetCursorPos(&realScreenPoint);
					gPanGestureBestRealPos = realScreenPoint;
				}
			}
		}
		if (kEnableDigitizerFix) {
			CURSORINFO cursorInfo;
			::memset(&cursorInfo, 0, sizeof(cursorInfo));
			cursorInfo.cbSize = sizeof(CURSORINFO);
			::GetCursorInfo(&cursorInfo);
			bool cursorShown = (cursorInfo.flags & CURSOR_SHOWING);
			int mx = GET_X_LPARAM(lParam);
			int my = GET_Y_LPARAM(lParam);
			bool isPenEvent = IS_PEN_EVENT(::GetMessageExtraInfo());
			bool isDoubleClick = gHadDoubleClick > 0;
			if (isDoubleClick) {
				gHadDoubleClick--;
			}
			DEBUG_TRACE(_T("MOUSEMOVE [%d, %d] Cursor: %s Pen: %s\n"), mx, my, cursorShown ? _T("true") : _T("false"), isPenEvent ? _T("true") : _T("false"));
			if (!cursorShown) {
				bool isCtrlOrShift = (wParam & MK_CONTROL) || (wParam & MK_SHIFT);
				bool isErrantEvent = isPenEvent && gHadDigitizerEvent && mx == gDragDownPoint.x && my == gDragDownPoint.y;
				if (isPenEvent && !isErrantEvent) {
					bool wasAcknowledged = !gHadDigitizerEvent;
					gHadDigitizerEvent = true;
					gWasDigitizerClick = true;
					if (isCtrlOrShift && !wasAcknowledged) {
						lParam = MAKELONG((short) gDragDownPoint.x, (short) gDragDownPoint.y);
						break;
					}
					int dx = mx - gDragPrevDigitizerPoint.x;
					int dy = my - gDragPrevDigitizerPoint.y;
					gDragPrevDigitizerPoint.x = mx;
					gDragPrevDigitizerPoint.y = my;
					gDragDigitizerDelta.x += dx;
					gDragDigitizerDelta.y += dy;
					int targetDX = gDragDigitizerDelta.x;
					int targetDY = gDragDigitizerDelta.y;
					if (!wasAcknowledged && ((std::abs(targetDX) < 3 && std::abs(targetDY) < 3) || (rand() % 100) > 70)) {
						lParam = MAKELONG((short) gDragDownPoint.x, (short) gDragDownPoint.y);
						break;
					}
					//if (wParam & MK_CONTROL) {
					//	targetDX = 0;
					//	targetDY = 0;
					//}
					int targetX = gDragDownPoint.x + targetDX;
					int targetY = gDragDownPoint.y + targetDY;
					// Tell Live the mouse moved a small distance from where the button was originally pressed.
					DEBUG_TRACE(_T("[%d, %d] -> [%d, %d]\n"), gDragDownPoint.x, gDragDownPoint.y, targetDX, targetDY);
					lParam = MAKELONG((short) targetX, (short) targetY);
					gDragDigitizerDelta.x = 0;
					gDragDigitizerDelta.y = 0;
				} else {
					if (gHadDigitizerEvent || gDragPenDown) {
						DEBUG_TRACE(_T("Reset: [%d, %d] Errant: %s\n"), gDragDownPoint.x, gDragDownPoint.y, isErrantEvent ? _T("true") : _T("false"));
						if (gHadDigitizerClick) {
							// Record the location Live thinks the mouse button was pressed.
							gHadDigitizerClick = false;
							gDragDownPoint.x = mx;
							gDragDownPoint.y = my;
						}
						// Live has acknowledged the mouse move, and forced the cursor back to where
						// it thinks the button went down. That means we can reset our running
						// delta.
						if (isCtrlOrShift) {
							gDragDigitizerDelta.x = 0;
							gDragDigitizerDelta.y = 0;
						//	gDragDownPoint.x = mx;
						//	gDragDownPoint.y = my;
							lParam = MAKELONG((short) gDragDownPoint.x, (short) gDragDownPoint.y);
						}
						gHadDigitizerEvent = false;
					//	lParam = MAKELONG((short) gDragDownPoint.x, (short) gDragDownPoint.y);
					} else {
						//gDragDownPoint.x = mx;
						//gDragDownPoint.y = my;
						//if (isCtrlOrShift) {
						//	gDragDigitizerDelta.x = 0;
						//	gDragDigitizerDelta.y = 0;
						//	//gDragDownPoint.x = mx;
						//	//gDragDownPoint.y = my;
						//	lParam = MAKELONG((short) gDragDownPoint.x, (short) gDragDownPoint.y);
						//}
					}
				}
			} else {
				if (isPenEvent) {
					if (!isDoubleClick) {
						gDragPrevDigitizerPoint.x = mx;
						gDragPrevDigitizerPoint.y = my;
					}
				}
			}
		}
		break;
	case WM_KEYDOWN:
		if (kEnableCtrlShiftZ && wParam == 'Z' && HIBYTE(::GetKeyState(VK_CONTROL)) && HIBYTE(::GetKeyState(VK_SHIFT))) {
			INPUT input[] = {
				MakeKeyboardEvent(VK_SHIFT, true),
				MakeKeyboardEvent('Y', false),
				MakeKeyboardEvent('Y', true),
				MakeKeyboardEvent(VK_SHIFT, false),
			};
			::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
		}
		break;

	case WM_CHAR:
	// The following conflicts with text editing, so turn it off if you like typing equals.
		if (kEnablePlusKeyWithoutShift && wParam == '=') {
			// Allow zooming without needing to hold shift.
			wParam = '+';
		}
		break;

	case WM_GESTURENOTIFY:
		if (kEnableTouchPanGesture) {
			GESTURECONFIG gc;
			::memset(&gc, 0, sizeof(gc));
			gc.dwID = GID_PAN;
			gc.dwWant = GC_PAN | GC_PAN_WITH_INERTIA | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY;
			gc.dwBlock = 0;
			::SetGestureConfig(hwnd, 0, 1, &gc, sizeof(GESTURECONFIG));
		}
		break;

	case WM_GESTURE:
		if (kEnableTouchPanGesture) {
			GESTUREINFO gesture;
			::memset(&gesture, 0, sizeof(gesture));
			gesture.cbSize = sizeof(gesture);
			bool consumed = false;
			if (::GetGestureInfo((HGESTUREINFO) lParam, &gesture)) {
				if (gesture.dwID == GID_PAN) {
					// Fake a smooth pan by Ctrl+Alt clicking and dragging.
					DEBUG_TRACE(_T("GID_PAN: %d %ld, [%d, %d]\n"), gesture.dwFlags, gesture.ullArguments, gesture.ptsLocation.x, gesture.ptsLocation.y);
					bool restartedGesture = gPanGestureInInertia && gesture.dwFlags == 0;
					if (restartedGesture || (gesture.dwFlags & GF_BEGIN)) {
						// Cancel our current Ctrl+Alt click if it was already down.
						if (gPanGestureDown) {
							::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, WM_LBUTTONUP, 0, MAKELONG(gPanGestureLastPos.x, gPanGestureLastPos.y));
						}
						// Simulate a Ctrl+Alt click.
						gInPanGesture = true;
						gPanGestureDown = true;
						gPanGestureInInertia = false;
						{
							INPUT input[] = {
								MakeKeyboardEvent(VK_CONTROL, false),
								MakeKeyboardEvent(VK_MENU, false),
							};
							::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
						}
						POINT screenTouchPoint = { gesture.ptsLocation.x, gesture.ptsLocation.y };
						::ClientToScreen(hwnd, &screenTouchPoint);
						::SetCursorPos(screenTouchPoint.x, screenTouchPoint.y);
						::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, WM_MOUSEMOVE, 0, MAKELONG(gesture.ptsLocation.x, gesture.ptsLocation.y));
						::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, WM_LBUTTONDOWN, 0, MAKELONG(gesture.ptsLocation.x, gesture.ptsLocation.y));
						{
							INPUT input[] = {
								MakeKeyboardEvent(VK_CONTROL, true),
								MakeKeyboardEvent(VK_MENU, true),
							};
							::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
						}
						gPanGestureLastPos.x = gesture.ptsLocation.x;
						gPanGestureLastPos.y = gesture.ptsLocation.y;
					}
					if (gesture.dwFlags & GF_INERTIA) {
						gPanGestureInInertia = true;
						gInPanGesture = false;
					}
					if (gesture.dwFlags & GF_END) {
						// Cancel our current Ctrl+Alt click because the gesture is completely over.
						gPanGestureInInertia = false;
						if (gPanGestureDown) {
							gPanGestureDown = false;
							::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, WM_LBUTTONUP, 0, MAKELONG(gesture.ptsLocation.x, gesture.ptsLocation.y));
							::SetCursorPos(gPanGestureBestRealPos.x, gPanGestureBestRealPos.y);
						}
					}

					if (!(gesture.dwFlags & GF_BEGIN) && gPanGestureDown) {
						::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, WM_MOUSEMOVE, 0, MAKELONG(gesture.ptsLocation.x, gesture.ptsLocation.y));
						gPanGestureLastPos.x = gesture.ptsLocation.x;
						gPanGestureLastPos.y = gesture.ptsLocation.y;
					}
					consumed = true;
				}
			}
			if (consumed) {
				::CloseGestureInfoHandle((HGESTUREINFO) lParam);
				return 0L;
			}
		}
		break;

	case WM_MOUSEWHEEL:
		if (kEnableTrackpadZoom && !gInTrackpadScroll && (loword == MK_CONTROL)) {
			// Either we got sent a touchpad zoom emulation event, or the user is scrolling while holding
			// control. We can't tell the difference, so just assume it's a zoom event.
			int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			int deltaAcc = gWheelZoomDeltaAcc + delta;
			int outDelta = deltaAcc / WHEEL_DELTA;
			deltaAcc -= outDelta * WHEEL_DELTA;
			gWheelZoomDeltaAcc = deltaAcc;
			if (outDelta != 0) {
				int deltas = std::abs(outDelta);
				DWORD zoomVKey = outDelta > 0 ? VK_OEM_PLUS : VK_OEM_MINUS;
				{ 
					INPUT input[] = {
						MakeKeyboardEvent(VK_CONTROL, true),
					};
					::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
				}
				for (int i = 0 ; i < deltas; i++) {
					INPUT input[] = {
						MakeKeyboardEvent(zoomVKey, false),
						MakeKeyboardEvent(zoomVKey, true),
					};
					::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
				}
				{
					INPUT input[] = {
						MakeKeyboardEvent(VK_CONTROL, false),
					};
					::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
				}
			}

			return 0L;
		} if (kEnableScrollingOverride) {
			int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			int outDelta = 0;
			int deltaAcc = gWheelDeltaAcc.y;
			deltaAcc += (delta * kWheelYScaleNumerator) / kWheelYScaleDenominator;
			outDelta = (deltaAcc / kMinWheelDelta) * kMinWheelDelta;
			gWheelDeltaAcc.y = deltaAcc - outDelta;

			if (outDelta == 0) {
				return 0L;
			}
			msg = WM_MOUSEWHEEL;
			wParam = MAKEWPARAM(loword, (short) outDelta);
		}
		break;
	case WM_MOUSEHWHEEL:
		if (kEnableScrollingOverride) {
			WORD loword = LOWORD(wParam);
			int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			int outDelta = 0;
			int deltaAcc = gWheelDeltaAcc.x;
			deltaAcc += (delta * -kWheelXScaleNumerator) / kWheelXScaleDenominator;
			outDelta = (deltaAcc / kMinWheelDelta) * kMinWheelDelta;
			gWheelDeltaAcc.x = deltaAcc - outDelta;

			if (outDelta == 0) {
				return 0L;
			}
			msg = WM_MOUSEWHEEL;
			wParam = MAKEWPARAM(MK_CONTROL, (short) outDelta);
			{
				INPUT input[] = {
					MakeKeyboardEvent(VK_CONTROL, false),
				};
				::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
			}
			gInTrackpadScroll = true;
			LRESULT ret = ::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, msg, wParam, lParam);
			gInTrackpadScroll = false;
			{
				INPUT input[] = {
					MakeKeyboardEvent(VK_CONTROL, true),
				};
				::SendInput(sizeof(input) / sizeof(INPUT), input, sizeof(INPUT));
			}
			return ret;
		}
		break;
	}

	// Forward the message.
	return ::CallWindowProc((WNDPROC) data.oldWndProc, hwnd, msg, wParam, lParam);
}

// Construct a keyboard event suitable for use with SendInput, to simulate key presses.
INPUT MakeKeyboardEvent(int vk, bool keyUp) {
	INPUT input;
	memset(&input, 0, sizeof(input));
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk;
	input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
	return input;
}


// Hook into a window by overriding its WndProc. This causes all messages to get forwarded to
// OurWndProc first.
void MapWindow(HWND hwnd) {
	std::lock_guard<std::recursive_mutex> lock(*gStateLock);
	auto findIt = gWindowMap->find(hwnd);
	if (findIt != gWindowMap->end()) {
		return;
	}
	LONG_PTR oldWndProc = ::GetWindowLongPtr(hwnd, GWLP_WNDPROC);
	WindowData data = { hwnd, oldWndProc };
	(*gWindowMap)[hwnd] = data;

	::SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR) OurWndProc);
}

// Stop overriding a window's WndProc.
void UnmapWindow(HWND hwnd) {
	std::lock_guard<std::recursive_mutex> lock(*gStateLock);
	auto findIt = gWindowMap->find(hwnd);
	if (findIt == gWindowMap->end()) {
		return;
	}
	auto data = findIt->second;
	gWindowMap->erase(findIt);

	// This might be incorrect by the time it gets to us.
	::SetWindowLongPtr(data.hwnd, GWLP_WNDPROC, data.oldWndProc);
}

// Stop overriding all known window WndProcs.
void UnmapAllWindows() {
	std::lock_guard<std::recursive_mutex> lock(*gStateLock);
	while (!gWindowMap->empty()) {
		UnmapWindow(gWindowMap->begin()->first);
	}
}

// Window hook for use with SetWinEventHook. All this does is hook in our real event handler, since
// window hooks cannot override events (they can only inspect them).
void CALLBACK OurWinEventProc(
		HWINEVENTHOOK hWinEventHook,
		DWORD         event,
		HWND          hwnd,
		LONG          idObject,
		LONG          idChild,
		DWORD         dwEventThread,
		DWORD         dwmsEventTime) {
	switch (event) {
	case WM_DESTROY:
		UnmapWindow(hwnd);
		break;
	case WM_KILLFOCUS:
		MapWindow(hwnd);
		break;
	default:
		break;
	}
}

} // namespace

// DLL entry point, which is called when Live scans the plugin. Since we don't actually export any
// VST functions, Live unloads us immediately, but this gives us a chance to install a window hook.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID lpReserved) {
	HWINEVENTHOOK hHook = nullptr;
	switch (ulReasonForCall) {
	case DLL_PROCESS_ATTACH:
		if (!gStateLock) {
			gStateLock = new std::recursive_mutex();
		}
		if (!gWindowMap) {
			gWindowMap = new std::unordered_map<HWND, WindowData>();
		}
		hHook = ::SetWinEventHook(WM_CREATE, WM_KILLFOCUS, hModule, OurWinEventProc, ::GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
		break;
	case DLL_PROCESS_DETACH:
		if (!lpReserved) {
			// DLL detaching, process stays around.
			UnmapAllWindows();
		}
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return TRUE;
}
