#include "std.h"
#include "sdlinput.h"
#include "sdlruntime.h"

#include <dinput.h>
#include <SDL3/SDL.h>

struct SDLGamepadBit {
	SDL_GamepadButton button;
	int bit;
};

static const SDLGamepadBit gamepad_bits[] = {
	{ SDL_GAMEPAD_BUTTON_DPAD_UP, 0 },
	{ SDL_GAMEPAD_BUTTON_DPAD_DOWN, 1 },
	{ SDL_GAMEPAD_BUTTON_DPAD_LEFT, 2 },
	{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 3 },
	{ SDL_GAMEPAD_BUTTON_START, 4 },
	{ SDL_GAMEPAD_BUTTON_BACK, 5 },
	{ SDL_GAMEPAD_BUTTON_LEFT_STICK, 6 },
	{ SDL_GAMEPAD_BUTTON_RIGHT_STICK, 7 },
	{ SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 8 },
	{ SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 9 },
	{ SDL_GAMEPAD_BUTTON_SOUTH, 12 },
	{ SDL_GAMEPAD_BUTTON_EAST, 13 },
	{ SDL_GAMEPAD_BUTTON_WEST, 14 },
	{ SDL_GAMEPAD_BUTTON_NORTH, 15 }
};

class Device : public sdlDevice {
public:
    bool acquired;
    sdlInput* input;
    IDirectInputDevice8* device;

    Device(sdlInput* i, IDirectInputDevice8* d) :input(i), acquired(false), device(d) {
    }
    virtual ~Device() {
        if (device) device->Release();
    }
    bool acquire() {
        if (device) return acquired = device->Acquire() >= 0;
        return false;
    }
    void unacquire() {
        if (device) device->Unacquire();
        acquired = false;
    }
};

class Keyboard : public Device {
public:
    Keyboard(sdlInput* i, IDirectInputDevice8* d) :Device(i, d) {
    }
    void update() {

        if (!device) return;

        if (!acquired) {
            input->runtime->idle();
            return;
        }
        int k, cnt = 32;
        DIDEVICEOBJECTDATA data[32], * curr;
        if (device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, (DWORD*)&cnt, 0) < 0) return;
        curr = data;
        for (k = 0; k < cnt; ++curr, ++k) {
            int n = curr->dwOfs; if (!n || n > 255) continue;
            if (curr->dwData & 0x80) downEvent(n);
            else upEvent(n);
        }
    }
};

class Mouse : public Device {    // not being used???
public:
    Mouse(sdlInput* i, IDirectInputDevice8* d) : Device(i, d) {
    }
    void update() {
        if (!device) return;

        if (!acquired) {
            input->runtime->idle();
            return;
        }
        DIMOUSESTATE2 state;
        if (device->GetDeviceState(sizeof(state), &state) < 0) return;
        if (sdlGraphics* g = input->runtime->graphics) {
            int mx = axis_states[0] + state.lX;
            int my = axis_states[1] + state.lY;
            if (mx < 0) mx = 0;
            else if (mx >= g->getWidth()) mx = g->getWidth() - 1;
            if (my < 0) my = 0;
            else if (my >= g->getHeight()) my = g->getHeight() - 1;
            axis_states[0] = mx;
            axis_states[1] = my;
            axis_states[2] += state.lZ;
        }
        for (int k = 0; k < 8; ++k) {
            setDownState(k + 1, state.rgbButtons[k] & 0x80);
        }
    }
};

class SDLInputDevice : public sdlDevice {
public:
    sdlInput* input;
    int type;
    bool connected;
    bool is_gamepad;
    unsigned instance_id;
    unsigned poll_time;
    SDL_Gamepad* gamepad;
    SDL_Joystick* joystick;

    SDLInputDevice(sdlInput* i, SDL_Gamepad* gp) :input(i), type(3), connected(true), is_gamepad(true),
        instance_id(SDL_GetGamepadID(gp)), poll_time(0), gamepad(gp), joystick(0) {
        reset();
    }
    SDLInputDevice(sdlInput* i, SDL_Joystick* js) :input(i), type(SDL_GetJoystickType(js) == SDL_JOYSTICK_TYPE_GAMEPAD ? 1 : 2),
        connected(true), is_gamepad(false), instance_id(SDL_GetJoystickID(js)), poll_time(0), gamepad(0), joystick(js) {
        reset();
    }
    virtual ~SDLInputDevice() {
        close();
    }
    void close() {
        if (gamepad) { SDL_CloseGamepad(gamepad); gamepad = 0; }
        if (joystick) { SDL_CloseJoystick(joystick); joystick = 0; }
    }
    void clearState() {
        if (is_gamepad) {
            for (int k = 0; k < 23; ++k) setDownState(k, false);
        }
        else {
            for (int k = 0; k < 31; ++k) setDownState(k + 1, false);
        }
        for (int k = 0; k < 9; ++k) axis_states[k] = 0;
        axis_states[8] = -1;
    }
    void disconnect() {
        if (!connected) return;
        connected = false;
        clearState();
        close();
    }
    void update() {
        if (is_gamepad) updateGamepad();
        else updateJoystick();
    }
    void updateGamepad() {
        input->pumpEvents(false);
        if (!connected || !gamepad) return;
        float lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float ly = -(float)SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        float rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float ry = -(float)SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        float lt = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) + 32768) / 65535.0f;
        float rt = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) + 32768) / 65535.0f;
        axis_states[0] = lx;
        axis_states[1] = ly;
        axis_states[2] = lt - rt;
        axis_states[3] = rx;
        axis_states[4] = ry;
        axis_states[5] = lt;
        axis_states[6] = rt;
        Uint16 buttons = 0;
        for (size_t k = 0; k < sizeof(gamepad_bits) / sizeof(gamepad_bits[0]); ++k) {
            if (SDL_GetGamepadButton(gamepad, gamepad_bits[k].button)) buttons |= 1 << gamepad_bits[k].bit;
        }
        for (int k = 0; k < 23; ++k) {
            setDownState(k, (buttons & (1 << k)) ? true : false);
        }
        bool up = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool down = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        bool left = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        bool right = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        int pov = -1;
        if (up) {
            if (right) pov = 45;
            else if (left) pov = 315;
            else pov = 0;
        }
        else if (down) {
            if (right) pov = 135;
            else if (left) pov = 225;
            else pov = 180;
        }
        else if (right) {
            pov = 90;
        }
        else if (left) {
            pov = 270;
        }
        axis_states[8] = pov;
    }
    void updateJoystick() {
        unsigned tm = timeGetTime();
        if (tm - poll_time < 3) return;
        poll_time = tm;
        input->pumpEvents(false);
        if (!connected || !joystick) return;
        int axes = SDL_GetNumJoystickAxes(joystick);
        if (axes > 8) axes = 8;
        for (int k = 0; k < axes; ++k) {
            float t = SDL_GetJoystickAxis(joystick, k) / 32767.5f;
            if (t < -1) t = -1;
            else if (t > 1) t = 1;
            axis_states[k] = t;
        }
        if (SDL_GetNumJoystickHats(joystick) > 0) {
            switch (SDL_GetJoystickHat(joystick, 0)) {
            case SDL_HAT_UP: axis_states[8] = 0; break;
            case SDL_HAT_RIGHTUP: axis_states[8] = 45; break;
            case SDL_HAT_RIGHT: axis_states[8] = 90; break;
            case SDL_HAT_RIGHTDOWN: axis_states[8] = 135; break;
            case SDL_HAT_DOWN: axis_states[8] = 180; break;
            case SDL_HAT_LEFTDOWN: axis_states[8] = 225; break;
            case SDL_HAT_LEFT: axis_states[8] = 270; break;
            case SDL_HAT_LEFTUP: axis_states[8] = 315; break;
            default: axis_states[8] = -1; break;
            }
        }
        else {
            axis_states[8] = -1;
        }
        int buttons = SDL_GetNumJoystickButtons(joystick);
        if (buttons > 31) buttons = 31;
        for (int k = 0; k < buttons; ++k) {
            setDownState(k + 1, SDL_GetJoystickButton(joystick, k) ? true : false);
        }
    }
};

static Keyboard* keyboard;
static Mouse* mouse;
static std::vector<int> chars;

static Keyboard* createKeyboard(sdlInput* input) {

    return new Keyboard(input, 0);
}

static Mouse* createMouse(sdlInput* input) {
    LPDIRECTINPUTDEVICE8 dev;
    if (input->dirInput->CreateDevice(GUID_SysMouse, &dev, 0) >= 0) {
        if (dev->SetCooperativeLevel(input->runtime->hwnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE) >= 0) {
            if (dev->SetDataFormat(&c_dfDIMouse2) >= 0) {
                return new Mouse(input, dev);
            }
        }
        dev->Release();
    }
    return new Mouse(input, 0);
}

SDLInputDevice* sdlInput::findDevice(int port) {
    return port >= 0 && port < (int)sdl_devices.size() ? sdl_devices[port] : 0;
}

SDLInputDevice* sdlInput::findByInstance(unsigned id) {
    for (size_t k = 0; k < sdl_devices.size(); ++k) {
        if (sdl_devices[k]->connected && sdl_devices[k]->instance_id == id) return sdl_devices[k];
    }
    return 0;
}

SDLInputDevice* sdlInput::addGamepad(unsigned id) {
    if (!sdl_ok || findByInstance(id)) return 0;
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    if (!gp) return 0;
    SDLInputDevice* d = new SDLInputDevice(this, gp);
    sdl_devices.insert(sdl_devices.begin() + gamepad_count, d);
    ++gamepad_count;
    return d;
}

SDLInputDevice* sdlInput::addJoystick(unsigned id) {
    if (!sdl_ok || findByInstance(id)) return 0;
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) return 0;
    SDLInputDevice* d = new SDLInputDevice(this, js);
    sdl_devices.push_back(d);
    return d;
}

void sdlInput::disconnectDevice(unsigned id) {
    SDLInputDevice* d = findByInstance(id);
    if (d) d->disconnect();
}

void sdlInput::pumpEvents(bool force) {
    if (!sdl_ok) return;
    unsigned tm = timeGetTime();
    if (!force && tm - last_pump < 3) return;
    last_pump = tm;
    SDL_PumpEvents();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            addGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_JOYSTICK_ADDED:
            if (!SDL_IsGamepad(ev.jdevice.which)) addJoystick(ev.jdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            disconnectDevice(ev.gdevice.which);
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            disconnectDevice(ev.jdevice.which);
            break;
        }
    }
}

sdlInput::sdlInput(sdlRuntime* rt, IDirectInput8* di) :
    runtime(rt), dirInput(di), gamepad_count(0), sdl_ok(false), last_pump(0) {
    keyboard = createKeyboard(this);
    mouse = createMouse(this);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    sdl_ok = SDL_Init(SDL_INIT_GAMEPAD);
    if (!sdl_ok) {
        runtime->debugLog("Failed to initialize SDL input.");
        return;
    }

    int n = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&n);
    for (int k = 0; pads && k < n; ++k) addGamepad(pads[k]);
    SDL_free(pads);

    SDL_JoystickID* sticks = SDL_GetJoysticks(&n);
    for (int k = 0; sticks && k < n; ++k) {
        if (!findByInstance(sticks[k])) addJoystick(sticks[k]);
    }
    SDL_free(sticks);
}

sdlInput::~sdlInput() {
    for (size_t i = 0; i < sdl_devices.size(); ++i) delete sdl_devices[i];
    sdl_devices.clear();
    gamepad_count = 0;

    if (sdl_ok) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        sdl_ok = false;
    }

    delete mouse;
    delete keyboard;

    dirInput->Release();
}

void sdlInput::wm_keydown(int key) {
    if (keyboard) keyboard->downEvent(key);
}

void sdlInput::wm_keyup(int key) {
    if (keyboard) keyboard->upEvent(key);
}

void sdlInput::wm_mousedown(int key) {
    if (mouse) mouse->downEvent(key);
}

void sdlInput::wm_mouseup(int key) {
    if (mouse) mouse->upEvent(key);
}

void sdlInput::wm_mousemove(int x, int y) {
    if (mouse) {
        mouse->axis_states[0] = x;
        mouse->axis_states[1] = y;
    }
}

void sdlInput::wm_mousewheel(int dz) {
    if (mouse) mouse->axis_states[2] += dz;
}

void sdlInput::wm_char(int wParam, int lParam) {
    int repeats = lParam & 0xffff;
    for (int i = 0; i < repeats; i++) {
        chars.push_back(wParam);
    }
}

void sdlInput::reset() {
    if (mouse) mouse->reset();
    if (keyboard) keyboard->reset();
    for (size_t k = 0; k < sdl_devices.size(); ++k) sdl_devices[k]->reset();
}

bool sdlInput::acquire() {
    bool m_ok = mouse ? mouse->acquire() : false;
    bool k_ok = keyboard ? keyboard->acquire() : false;
    if (m_ok && k_ok) return true;
    if (k_ok && keyboard) keyboard->unacquire();
    if (m_ok && mouse) mouse->unacquire();
    return false;
}

void sdlInput::unacquire() {
    if (keyboard) keyboard->unacquire();
    if (mouse) mouse->unacquire();
}

void sdlInput::moveMouse(int x, int y) {
    if (!mouse) return;
    mouse->axis_states[0] = x;
    mouse->axis_states[1] = y;
    runtime->moveMouse(x, y);
}

sdlDevice* sdlInput::getMouse()const {
    return mouse;
}

sdlDevice* sdlInput::getKeyboard()const {
    return keyboard;
}

bool sdlInput::getControllerConnected(int port) {
    pumpEvents(false);
    SDLInputDevice* d = findDevice(port);
    return d && d->connected;
}

sdlDevice* sdlInput::getJoystick(int n)const {
    return n >= 0 && n < (int)sdl_devices.size() ? sdl_devices[n] : 0;
}

std::vector<int> sdlInput::getChars() {
    std::vector<int> chrs = chars;
    chars.clear();
    return chrs;
}

int sdlInput::getJoystickType(int n)const {
    return n >= 0 && n < (int)sdl_devices.size() ? sdl_devices[n]->type : 0;
}

int sdlInput::numJoysticks()const {
    return (int)sdl_devices.size();
}

bool sdlInput::rumble(int port, float left, float right) {
    SDLInputDevice* d = findDevice(port);
    if (!d || !d->connected) return false;
    if (left < 0) left = 0;
    else if (left > 1) left = 1;
    if (right < 0) right = 0;
    else if (right > 1) right = 1;
    Uint16 lo = (Uint16)(left * 65535.0f);
    Uint16 hi = (Uint16)(right * 65535.0f);
    if (d->is_gamepad) return SDL_RumbleGamepad(d->gamepad, lo, hi, 0xFFFFFFFF);
    return SDL_RumbleJoystick(d->joystick, lo, hi, 0xFFFFFFFF);
}

int sdlInput::toUnicode(int scan)const {
    switch (scan) {
    case DIK_INSERT:return ASC_INSERT;
    case DIK_DELETE:return ASC_DELETE;
    case DIK_HOME:return ASC_HOME;
    case DIK_END:return ASC_END;
    case DIK_PGUP:return ASC_PAGEUP;
    case DIK_PGDN:return ASC_PAGEDOWN;
    case DIK_UP:return ASC_UP;
    case DIK_DOWN:return ASC_DOWN;
    case DIK_LEFT:return ASC_LEFT;
    case DIK_RIGHT:return ASC_RIGHT;
    }
    scan &= 0x7f;
    int virt = MapVirtualKey(scan, 1);
    if (!virt) return 0;

    static unsigned char mat[256];
    mat[VK_LSHIFT] = keyboard->keyDown(DIK_LSHIFT) ? 0x80 : 0;
    mat[VK_RSHIFT] = keyboard->keyDown(DIK_RSHIFT) ? 0x80 : 0;
    mat[VK_SHIFT] = mat[VK_LSHIFT] | mat[VK_RSHIFT];
    mat[VK_LCONTROL] = keyboard->keyDown(DIK_LCONTROL) ? 0x80 : 0;
    mat[VK_RCONTROL] = keyboard->keyDown(DIK_RCONTROL) ? 0x80 : 0;
    mat[VK_CONTROL] = mat[VK_LCONTROL] | mat[VK_RCONTROL];
    mat[VK_LMENU] = keyboard->keyDown(DIK_LMENU) ? 0x80 : 0;
    mat[VK_RMENU] = keyboard->keyDown(DIK_RMENU) ? 0x80 : 0;
    mat[VK_MENU] = mat[VK_LMENU] | mat[VK_RMENU];

    WCHAR ch[4];
    if (!ToUnicode(virt, scan, mat, ch, 4, 0)) return 0;
    return (int)ch[0];
}

