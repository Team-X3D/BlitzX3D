#include "std.h"
#include "bbsys.h"
#include "../sdlruntime/sdlutf8.h"

sdlInput* sdl_input;
sdlDevice* sdl_mouse;
sdlDevice* sdl_keyboard;
std::vector<sdlDevice*> sdl_joysticks;

static int mouse_x, mouse_y, mouse_z;
static const float JLT = -1.0f / 3.0f;
static const float JHT = 1.0f / 3.0f;

bool input_create() {
	if (sdl_input = sdl_runtime->openInput(0)) {
		sdl_keyboard = sdl_input->getKeyboard();
		sdl_mouse = sdl_input->getMouse();
		sdl_joysticks.clear();
		for (int k = 0; k < sdl_input->numJoysticks(); ++k) {
			sdl_joysticks.push_back(sdl_input->getJoystick(k));
		}
		mouse_x = mouse_y = mouse_z = 0;
		return true;
	}
	return false;
}

bool input_destroy() {
	sdl_joysticks.clear();
	sdl_runtime->closeInput(sdl_input);
	sdl_input = 0;
	return true;
}

int bbKeyDown(int n) {
	if (!sdl_keyboard) return 0;
	return sdl_keyboard->keyDown(n);
}

int bbKeyHit(int n) {
	if (!sdl_keyboard) return 0;
	return sdl_keyboard->keyHit(n);
}

int bbGetKey() {
	if (!sdl_input || !sdl_keyboard) return 0;
	return sdl_input->toUnicode(sdl_keyboard->getKey());
}

BBStr* bbTextInput(BBStr* s) {
	BBStr t = *s;
	char tBuf[9];
	std::vector<int> chars = sdl_input->getChars();
	for (int i = 0; i < chars.size(); i++) {
		if (chars[i] == 8) { //backspace
			if (t.size() > 0) UTF8::popBack(t);
		}
		else if (chars[i] == 127) {
			t.clear();
		}
		else if (chars[i] >= 32) {
			int codepointLen = UTF8::encodeCharacter(chars[i], tBuf);
			tBuf[codepointLen] = '\0';
			t += tBuf;
		}
	}
	*s = t;
	return s;
}

int bbWaitKey() {
	for (;;) {
		if (!sdl_runtime->idle()) RTEX(0);
		if (sdl_keyboard) {
			if (int key = sdl_keyboard->getKey()) {
				if (key = sdl_input->toUnicode(key)) return key;
			}
		}
		sdl_runtime->delay(20);
	}
}

void bbFlushKeys() {
	sdl_input->getChars();
	if (sdl_keyboard) sdl_keyboard->flush();
}

int bbMouseDown(int n) {
	if (!sdl_mouse) return 0;
	return sdl_mouse->keyDown(n);
}

int bbMouseHit(int n) {
	if (!sdl_mouse) return 0;
	return sdl_mouse->keyHit(n);
}

int bbGetMouse() {
	if (!sdl_mouse) return 0;
	return sdl_mouse->getKey();
}

int bbWaitMouse() {
	for (;;) {
		if (!sdl_runtime->idle()) RTEX(0);
		if (sdl_mouse) {
			if (int key = sdl_mouse->getKey()) return key;
		}
		sdl_runtime->delay(20);
	}
}

int bbMouseWait() {
	return bbWaitMouse();
}

int bbMouseX() {
	return sdl_mouse ? sdl_mouse->getAxisState(0) : 0;
}

int bbMouseY() {
	return sdl_mouse ? sdl_mouse->getAxisState(1) : 0;
}

int bbMouseZ() {
	return sdl_mouse ? sdl_mouse->getAxisState(2) / 120 : 0;
}

int bbMouseXSpeed() {
	int dx = bbMouseX() - mouse_x;
	mouse_x += dx;
	return dx;
}

int bbMouseYSpeed() {
	int dy = bbMouseY() - mouse_y;
	mouse_y += dy;
	return dy;
}

int bbMouseZSpeed() {
	int dz = bbMouseZ() - mouse_z;
	mouse_z += dz;
	return dz;
}

void bbFlushMouse() {
	if (sdl_mouse) sdl_mouse->flush();
}

void bbMoveMouse(int x, int y) {
	sdl_input->moveMouse(mouse_x = x, mouse_y = y);
}

int bbJoyType(int port) {
	return sdl_input->getJoystickType(port);
}

int bbJoyConnected(int port) {
	if (!sdl_input) return 0;
	return sdl_input->getControllerConnected(port) ? 1 : 0;
}

int bbJoyDown(int n, int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->keyDown(n);
}

int bbJoyHit(int n, int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->keyHit(n);
}

int bbGetJoy(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->getKey();
}

int bbWaitJoy(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	for (;;) {
		if (!sdl_runtime->idle()) RTEX(0);
		if (int key = sdl_joysticks[port]->getKey()) return key;
		sdl_runtime->delay(20);
	}
}

float bbJoyX(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->getAxisState(0);
}

float bbJoyY(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->getAxisState(1);
}

float bbJoyZ(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->getAxisState(2);
}

float bbJoyU(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->getAxisState(3);
}

float bbJoyV(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_joysticks[port]->getAxisState(4);
}

float bbJoyPitch(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_input->getJoystickType(port) == 3 ?
		sdl_joysticks[port]->getAxisState(4) * 180 :
		sdl_joysticks[port]->getAxisState(5) * 180;
}

float bbJoyYaw(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_input->getJoystickType(port) == 3 ?
		sdl_joysticks[port]->getAxisState(3) * 180 :
		sdl_joysticks[port]->getAxisState(6) * 180;
}

float bbJoyRoll(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	return sdl_input->getJoystickType(port) == 3 ?
		sdl_joysticks[port]->getAxisState(2) * 90 :
		sdl_joysticks[port]->getAxisState(7) * 180;
}

int bbJoyHat(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return -1;
	return sdl_joysticks[port]->getAxisState(8);
}

int	bbJoyXDir(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	float t = sdl_joysticks[port]->getAxisState(0);
	return t < JLT ? -1 : (t > JHT ? 1 : 0);
}

int bbJoyYDir(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	float t = sdl_joysticks[port]->getAxisState(1);
	return t < JLT ? -1 : (t > JHT ? 1 : 0);
}

int	bbJoyZDir(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	float t = sdl_joysticks[port]->getAxisState(2);
	return t < JLT ? -1 : (t > JHT ? 1 : 0);
}

float bbJoyLeftTrigger(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	if (sdl_input->getJoystickType(port) == 3) {
		return sdl_joysticks[port]->getAxisState(5);
	}
	return sdl_joysticks[port]->getAxisState(2); // Default to Z of Joystick for compatibility support
}

float bbJoyRightTrigger(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	if (sdl_input->getJoystickType(port) == 3) {
		return sdl_joysticks[port]->getAxisState(6);
	}
	return 0; // No fallback, just return early
}

int	bbJoyUDir(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	float t = sdl_joysticks[port]->getAxisState(3);
	return t < JLT ? -1 : (t > JHT ? 1 : 0);
}

int	bbJoyVDir(int port) {
	if (port < 0 || port >= sdl_joysticks.size()) return 0;
	float t = sdl_joysticks[port]->getAxisState(4);
	return t < JLT ? -1 : (t > JHT ? 1 : 0);
}

void bbJoyVibrate(int port, float left, float right) {
	if (port < 0 || port >= sdl_joysticks.size()) return;
	sdl_input->rumble(port, left, right);
}

void bbStopJoyVibrate(int port) {
	bbJoyVibrate(port, 0, 0);
}

int bbJoyCount() {
	return sdl_input ? sdl_input->numJoysticks() : 0;
}

void bbFlushJoy() {
	for (int k = 0; k < sdl_joysticks.size(); ++k) sdl_joysticks[k]->flush();
}

void  bbEnableDirectInput(int enable) {
	sdl_runtime->enableDirectInput(!!enable);
}

int  bbDirectInputEnabled() {
	return sdl_runtime->directInputEnabled();
}

BBStr* bbGetKeyName(int key) {
	UINT vk = 0;

	UINT vkFromScan = MapVirtualKeyW((UINT)key, MAPVK_VSC_TO_VK);
	if (vkFromScan != 0) {
		vk = vkFromScan;
	}
	else {
		DWORD scan = MapVirtualKeyW((UINT)key, MAPVK_VK_TO_VSC);
		if (scan != 0) {
			vk = (UINT)key;
		}
		else {
			return new BBStr("");
		}
	}

	DWORD dwScan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	if (dwScan == 0) {
		return new BBStr("");
	}

	WORD scanCode = LOBYTE(dwScan);
	BOOL extended = (HIWORD(dwScan) & 0x01) != 0;
	LPARAM lParam = (scanCode << 16) | (extended ? 0x01000000 : 0);

	wchar_t buffer[128];
	if (GetKeyNameTextW(lParam, buffer, 128) == 0) {
		return new BBStr("");
	}

	int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
	if (utf8Len <= 0) {
		return new BBStr("");
	}
	std::string utf8(utf8Len, 0);
	WideCharToMultiByte(CP_UTF8, 0, buffer, -1, &utf8[0], utf8Len, nullptr, nullptr);
	utf8.pop_back();

	return new BBStr(utf8);
}

void input_link(void (*rtSym)(const char* sym, void* pc)) {
	rtSym("%KeyDown%key", bbKeyDown);
	rtSym("%KeyHit%key", bbKeyHit);
	rtSym("%GetKey", bbGetKey);
	rtSym("%WaitKey", bbWaitKey);
	rtSym("$TextInput$txt", bbTextInput);
	rtSym("FlushKeys", bbFlushKeys);

	rtSym("%MouseDown%button", bbMouseDown);
	rtSym("%MouseHit%button", bbMouseHit);
	rtSym("%GetMouse", bbGetMouse);
	rtSym("%WaitMouse", bbWaitMouse);
	rtSym("%MouseWait", bbWaitMouse);
	rtSym("%MouseX", bbMouseX);
	rtSym("%MouseY", bbMouseY);
	rtSym("%MouseZ", bbMouseZ);
	rtSym("%MouseXSpeed", bbMouseXSpeed);
	rtSym("%MouseYSpeed", bbMouseYSpeed);
	rtSym("%MouseZSpeed", bbMouseZSpeed);
	rtSym("FlushMouse", bbFlushMouse);
	rtSym("MoveMouse%x%y", bbMoveMouse);

	rtSym("%JoyType%port=0", bbJoyType);
	rtSym("%JoyDown%button%port=0", bbJoyDown);
	rtSym("%JoyHit%button%port=0", bbJoyHit);
	rtSym("%GetJoy%port=0", bbGetJoy);
	rtSym("%WaitJoy%port=0", bbWaitJoy);
	rtSym("%JoyCount", bbJoyCount);
	rtSym("%JoyConnected%port=0", bbJoyConnected);
	rtSym("%JoyWait%port=0", bbWaitJoy);
	rtSym("#JoyX%port=0", bbJoyX);
	rtSym("#JoyY%port=0", bbJoyY);
	rtSym("#JoyZ%port=0", bbJoyZ);
	rtSym("#JoyU%port=0", bbJoyU);
	rtSym("#JoyV%port=0", bbJoyV);
	rtSym("#JoyPitch%port=0", bbJoyPitch);
	rtSym("#JoyYaw%port=0", bbJoyYaw);
	rtSym("#JoyRoll%port=0", bbJoyRoll);
	rtSym("%JoyHat%port=0", bbJoyHat);
	rtSym("%JoyXDir%port=0", bbJoyXDir);
	rtSym("%JoyYDir%port=0", bbJoyYDir);
	rtSym("%JoyZDir%port=0", bbJoyZDir);
	rtSym("#JoyLeftTrigger%port=0", bbJoyLeftTrigger);
	rtSym("#JoyRightTrigger%port=0", bbJoyRightTrigger);
	rtSym("%JoyUDir%port=0", bbJoyUDir);
	rtSym("%JoyVDir%port=0", bbJoyVDir);
	rtSym("JoyVibrate%port#left#right", bbJoyVibrate);
	rtSym("StopJoyVibrate%port", bbStopJoyVibrate);
	rtSym("FlushJoy", bbFlushJoy);

	rtSym("EnableDirectInput%enable", bbEnableDirectInput);
	rtSym("%DirectInputEnabled", bbDirectInputEnabled);

	rtSym("$GetKeyName%key", bbGetKeyName);
}