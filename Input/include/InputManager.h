#pragma once
#include <GameInput.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include <mutex>

using namespace GameInput::v3;
namespace JLib {
    // Thin wrapper over GameInput v3's polling API. IGameInput/IGameInputReading are plain COM
    // objects -- every GetCurrentReading() call hands back a NEW reference that must be Release()d,
    // so this class owns that lifetime internally and only ever exposes plain bool/float queries.
    //
    // GameInput has no named-key enum (no "GameInputKeySpace") -- keyboard state is an array of
    // GameInputKeyState{scanCode, codePoint, virtualKey, isDeadKey}, one entry per key CURRENTLY
    // held down. IsKeyDown/Pressed/Released below take a Win32 virtual-key code (VK_SPACE, 'A', etc.)
    // and scan for a matching virtualKey, so callers don't need to touch GameInputKeyState directly.
    class InputManager
    {
    public:
        ~InputManager();

        // Releases the IGameInput COM/WinRT object and every gamepad device reference. MUST be called
        // explicitly BEFORE CoUninitialize() if the caller owns the COM apartment (see main.cpp) --
        // relying on ~InputManager() alone means it runs at the enclosing scope's closing brace, which
        // can be AFTER CoUninitialize() already tore the apartment down. Calling Release() on a
        // COM/WinRT object post-CoUninitialize is undefined behavior; GameInput fails fast on it
        // ("Fatal program exit requested"). Safe to call more than once (idempotent) and safe to skip
        // if Initialize() was never called or already failed.
        void Shutdown();

        bool Initialize();
        // Call once per frame, before querying any state. Diffs this frame's readings against last
        // frame's to derive Pressed/Released edges (GameInput only ever gives you a snapshot, not
        // edge events, for polled reads).
        void Update();

        bool IsKeyDown(uint8_t virtualKey) const;
        bool IsKeyPressed(uint8_t virtualKey) const;  // true only on the frame it transitions down
        bool IsKeyReleased(uint8_t virtualKey) const; // true only on the frame it transitions up

        // Actual screen-space cursor position (GameInputMouseState::absolutePositionX/Y) -- what
        // you want for "where is the cursor right now" (UI hit-testing, click position, etc.).
        DirectX::XMFLOAT2 GetMousePos() const {
            return { (float)m_MouseState.absolutePositionX, (float)m_MouseState.absolutePositionY };
        }
        // Relative delta SINCE THE LAST READING (GameInputMouseState::positionX/Y) -- NOT position.
        // Good for camera-look style accumulation; use GetMousePos() if you want where the cursor is.
        float GetMouseDeltaX() const { return (float)m_MouseState.positionX; }
        float GetMouseDeltaY() const { return (float)m_MouseState.positionY; }

        bool IsMouseButtonDown(GameInputMouseButtons button) const;
        bool IsMouseButtonPressed(GameInputMouseButtons button) const;  // true only on the click frame
        bool IsMouseButtonReleased(GameInputMouseButtons button) const; // true only on the release frame

        // Single-controller convenience -- operates on gamepad index 0 (the "primary" pad, whichever
        // connected first). Equivalent to calling the indexed overloads below with gamepadIndex=0.
        bool IsButtonDown(GameInputGamepadButtons button) const { return IsButtonDown(0, button); }
        bool IsButtonPressed(GameInputGamepadButtons button) const { return IsButtonPressed(0, button); }
        bool IsButtonReleased(GameInputGamepadButtons button) const { return IsButtonReleased(0, button); }
        float GetLeftTriggerAxis() const { return GetLeftTriggerAxis(0); }
        float GetRightTriggerAxis() const { return GetRightTriggerAxis(0); }
        float GetLeftStickX() const { return GetLeftStickX(0); }
        float GetLeftStickY() const { return GetLeftStickY(0); }
        float GetRightStickX() const { return GetRightStickX(0); }
        float GetRightStickY() const { return GetRightStickY(0); }

        // Multi-controller: gamepadIndex is a STABLE slot assigned in connection order (0 = first
        // pad ever connected this run), not a GameInput device identity -- a disconnected pad's
        // slot is cleared in place (see Update()), not removed/reindexed, so held indices from
        // gameplay code (e.g. "player 2 is slot 1") never silently point at a different physical
        // controller after someone unplugs an earlier one. Out-of-range/disconnected index reads
        // as all-neutral (buttons up, sticks/triggers at 0) rather than throwing.
        bool IsButtonDown(uint32_t gamepadIndex, GameInputGamepadButtons button) const;
        bool IsButtonPressed(uint32_t gamepadIndex, GameInputGamepadButtons button) const;
        bool IsButtonReleased(uint32_t gamepadIndex, GameInputGamepadButtons button) const;
        float GetLeftTriggerAxis(uint32_t gamepadIndex) const;
        float GetRightTriggerAxis(uint32_t gamepadIndex) const;
        float GetLeftStickX(uint32_t gamepadIndex) const;
        float GetLeftStickY(uint32_t gamepadIndex) const;
        float GetRightStickX(uint32_t gamepadIndex) const;
        float GetRightStickY(uint32_t gamepadIndex) const;
        // True if gamepadIndex refers to a currently-connected pad (vs. an empty/disconnected slot).
        bool IsGamepadConnected(uint32_t gamepadIndex) const;
        uint32_t GetConnectedGamepadCount() const;
    private:
        IGameInput* m_GameInput = nullptr;
        GameInputCallbackToken m_DeviceCallbackToken = 0;

        // One slot per gamepad, in FIRST-CONNECTED order (see the class-level comment on the
        // indexed accessors for why slots are cleared-in-place rather than erased on disconnect).
        struct GamepadSlot {
            IGameInputDevice* device = nullptr; // nullptr = empty/disconnected slot
            GameInputGamepadState state = {};
            GameInputGamepadButtons prevButtons = GameInputGamepadNone;
        };
        std::vector<GamepadSlot> m_Gamepads;
        // GameInput does NOT guarantee OnGamepadDeviceChanged runs on the calling thread -- it can
        // fire on an arbitrary system thread for a hotplug event, concurrently with Update() (called
        // every frame on the main thread) or any of the indexed accessors below, all of which read
        // m_Gamepads without this lock. A push_back reallocating m_Gamepads' buffer while another
        // thread is mid-iteration over the old buffer is a real, previously-unsynchronized data race
        // that corrupted heap/stack memory near InputManager unpredictably (the exact symptom depended
        // on where InputManager itself happened to live, not on any single bug site) -- every access
        // to m_Gamepads must hold this.
        mutable std::mutex m_GamepadsMutex;
        // RegisterDeviceCallback's context is a raw void* -- this is the trampoline GameInput calls
        // on ANY GameInputKindGamepad connect/disconnect (and, via BLOCKING enumeration at
        // registration time in Initialize(), once synchronously per ALREADY-connected pad too, so
        // this one registration both discovers what's plugged in now AND tracks hotplug going
        // forward -- no separate poll-for-new-devices step needed).
        static void CALLBACK OnGamepadDeviceChanged(
            GameInputCallbackToken callbackToken, void* context, IGameInputDevice* device,
            uint64_t timestamp, GameInputDeviceStatus currentStatus, GameInputDeviceStatus previousStatus);

        // This frame's / last frame's held-key virtual-key codes -- diffed each Update() to derive
        // Pressed/Released. A std::vector (not a fixed array) since GetKeyState's count is dynamic.
        std::vector<uint8_t> m_KeysDown;
        std::vector<uint8_t> m_PrevKeysDown;

        GameInputMouseState m_MouseState = {};
        GameInputMouseButtons m_PrevMouseButtons = GameInputMouseNone;

        static bool Contains(const std::vector<uint8_t>& keys, uint8_t virtualKey);
    };
}