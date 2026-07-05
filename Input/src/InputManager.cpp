#include "../include/InputManager.h"
#include <algorithm>

InputManager::~InputManager()
{
    Shutdown();
}

void InputManager::Shutdown()
{
    if (m_GameInput)
    {
        // UnregisterCallback blocks until any in-flight OnGamepadDeviceChanged invocation
        // completes, so by the time it returns no other thread can still be touching
        // m_Gamepads -- safe to walk it below without the lock.
        if (m_DeviceCallbackToken) m_GameInput->UnregisterCallback(m_DeviceCallbackToken);
        m_DeviceCallbackToken = 0;
        for (auto& slot : m_Gamepads)
            if (slot.device) slot.device->Release();
        m_Gamepads.clear();
        m_GameInput->Release();
        m_GameInput = nullptr; // makes this safe to call again from ~InputManager()
    }
}

bool InputManager::Initialize()
{
    // GameInputCreate is the inline convenience wrapper (see GameInput.h) around
    // GameInputInitialize(IID_IGameInput, ...) -- returns a real COM reference we own until
    // Release() in the destructor.
    HRESULT hr = GameInputCreate(&m_GameInput);
    if (FAILED(hr)) return false;

    // GameInputBlockingEnumeration makes this call itself synchronously invoke
    // OnGamepadDeviceChanged once for every gamepad ALREADY connected, before returning --
    // so this one registration both discovers what's plugged in right now and keeps tracking
    // hotplug connect/disconnect for the rest of the program's life.
    HRESULT regHr = m_GameInput->RegisterDeviceCallback(
        nullptr, GameInputKindGamepad, GameInputDeviceAnyStatus, GameInputBlockingEnumeration,
        this, &InputManager::OnGamepadDeviceChanged, &m_DeviceCallbackToken);
    return SUCCEEDED(regHr);
}

void CALLBACK InputManager::OnGamepadDeviceChanged(
    GameInputCallbackToken /*callbackToken*/, void* context, IGameInputDevice* device,
    uint64_t /*timestamp*/, GameInputDeviceStatus currentStatus, GameInputDeviceStatus previousStatus)
{
    auto* self = static_cast<InputManager*>(context);
    bool isConnected = (currentStatus & GameInputDeviceConnected) != 0;
    bool wasConnected = (previousStatus & GameInputDeviceConnected) != 0;

    std::lock_guard<std::mutex> lock(self->m_GamepadsMutex);
    if (isConnected && !wasConnected)
    {
        // Reuse an existing empty slot (a previously-disconnected pad's spot) before appending a
        // new one, so a reconnect fills the gap instead of growing the vector unboundedly over
        // a long play session of repeated plug/unplug.
        for (auto& slot : self->m_Gamepads)
        {
            if (!slot.device)
            {
                device->AddRef();
                slot.device = device;
                slot.state = {};
                slot.prevButtons = GameInputGamepadNone;
                return;
            }
        }
        device->AddRef();
        InputManager::GamepadSlot slot;
        slot.device = device;
        self->m_Gamepads.push_back(slot);
    }
    else if (!isConnected && wasConnected)
    {
        // Clear IN PLACE, don't erase -- see the indexed-accessor comment in the header for why
        // slot indices must stay stable across a disconnect.
        for (auto& slot : self->m_Gamepads)
        {
            if (slot.device == device)
            {
                slot.device->Release();
                slot = InputManager::GamepadSlot{};
                break;
            }
        }
    }
}

bool InputManager::Contains(const std::vector<uint8_t>& keys, uint8_t virtualKey)
{
    return std::find(keys.begin(), keys.end(), virtualKey) != keys.end();
}

void InputManager::Update()
{
    m_PrevKeysDown = m_KeysDown;
    m_KeysDown.clear();
    m_PrevMouseButtons = m_MouseState.buttons;
    {
        std::lock_guard<std::mutex> lock(m_GamepadsMutex);
        for (auto& slot : m_Gamepads)
            slot.prevButtons = slot.state.buttons;
    }

    // --- Keyboard ---
    IGameInputReading* keyboardReading = nullptr;
    if (SUCCEEDED(m_GameInput->GetCurrentReading(GameInputKindKeyboard, nullptr, &keyboardReading)))
    {
        uint32_t keyCount = keyboardReading->GetKeyCount();
        if (keyCount > 0)
        {
            std::vector<GameInputKeyState> states(keyCount);
            uint32_t written = keyboardReading->GetKeyState(keyCount, states.data());
            m_KeysDown.reserve(written);
            for (uint32_t i = 0; i < written; ++i)
                m_KeysDown.push_back(states[i].virtualKey);
        }
        keyboardReading->Release();
    }

    // --- Mouse ---
    IGameInputReading* mouseReading = nullptr;
    if (SUCCEEDED(m_GameInput->GetCurrentReading(GameInputKindMouse, nullptr, &mouseReading)))
    {
        mouseReading->GetMouseState(&m_MouseState);
        mouseReading->Release();
    }

    // --- Gamepads --- one reading PER connected device (passing that specific IGameInputDevice*,
    // not nullptr) -- nullptr would merge every pad into one reading, which is exactly the
    // single-controller behavior this replaces. OnGamepadDeviceChanged (registered in
    // Initialize()) is what keeps m_Gamepads in sync with connect/disconnect; this just polls
    // whichever slots are currently populated.
    {
        std::lock_guard<std::mutex> lock(m_GamepadsMutex);
        for (auto& slot : m_Gamepads)
        {
            if (!slot.device) continue;

            IGameInputReading* gamepadReading = nullptr;
            if (SUCCEEDED(m_GameInput->GetCurrentReading(GameInputKindGamepad, slot.device, &gamepadReading)))
            {
                gamepadReading->GetGamepadState(&slot.state);
                gamepadReading->Release();
            }
            else
            {
                slot.state = {};
            }
        }
    }
}

bool InputManager::IsKeyDown(uint8_t virtualKey) const
{
    return Contains(m_KeysDown, virtualKey);
}

bool InputManager::IsKeyPressed(uint8_t virtualKey) const
{
    return Contains(m_KeysDown, virtualKey) && !Contains(m_PrevKeysDown, virtualKey);
}

bool InputManager::IsKeyReleased(uint8_t virtualKey) const
{
    return !Contains(m_KeysDown, virtualKey) && Contains(m_PrevKeysDown, virtualKey);
}

bool InputManager::IsMouseButtonDown(GameInputMouseButtons button) const
{
    return (m_MouseState.buttons & button) != 0;
}

bool InputManager::IsMouseButtonPressed(GameInputMouseButtons button) const
{
    return (m_MouseState.buttons & button) && !(m_PrevMouseButtons & button);
}

bool InputManager::IsMouseButtonReleased(GameInputMouseButtons button) const
{
    return !(m_MouseState.buttons & button) && (m_PrevMouseButtons & button);
}

bool InputManager::IsButtonDown(uint32_t gamepadIndex, GameInputGamepadButtons button) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size() || !m_Gamepads[gamepadIndex].device) return false;
    return (m_Gamepads[gamepadIndex].state.buttons & button) != 0;
}

bool InputManager::IsButtonPressed(uint32_t gamepadIndex, GameInputGamepadButtons button) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size() || !m_Gamepads[gamepadIndex].device) return false;
    const auto& slot = m_Gamepads[gamepadIndex];
    return (slot.state.buttons & button) && !(slot.prevButtons & button);
}

bool InputManager::IsButtonReleased(uint32_t gamepadIndex, GameInputGamepadButtons button) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size() || !m_Gamepads[gamepadIndex].device) return false;
    const auto& slot = m_Gamepads[gamepadIndex];
    return !(slot.state.buttons & button) && (slot.prevButtons & button);
}

float InputManager::GetLeftTriggerAxis(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size()) return 0.0f;
    return m_Gamepads[gamepadIndex].state.leftTrigger;
}

float InputManager::GetRightTriggerAxis(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size()) return 0.0f;
    return m_Gamepads[gamepadIndex].state.rightTrigger;
}

float InputManager::GetLeftStickX(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size()) return 0.0f;
    return m_Gamepads[gamepadIndex].state.leftThumbstickX;
}

float InputManager::GetLeftStickY(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size()) return 0.0f;
    return m_Gamepads[gamepadIndex].state.leftThumbstickY;
}

float InputManager::GetRightStickX(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size()) return 0.0f;
    return m_Gamepads[gamepadIndex].state.rightThumbstickX;
}

float InputManager::GetRightStickY(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    if (gamepadIndex >= m_Gamepads.size()) return 0.0f;
    return m_Gamepads[gamepadIndex].state.rightThumbstickY;
}

bool InputManager::IsGamepadConnected(uint32_t gamepadIndex) const
{
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    return gamepadIndex < m_Gamepads.size() && m_Gamepads[gamepadIndex].device != nullptr;
}

uint32_t InputManager::GetConnectedGamepadCount() const {
    std::lock_guard<std::mutex> lock(m_GamepadsMutex);
    uint32_t count = 0;
    for (auto& slot : m_Gamepads)
        if (slot.device) count++;
    return count;
}