/*/#include "../include/InputManager.h"
#include <Windows.h>
#include <iostream>

int main()
{
    InputManager input;
    if (!input.Initialize())
    {
        std::cout << "Failed to initialize GameInput -- is the GameInput runtime installed?\n";
        return 1;
    }

    std::cout << "Polling input for 10 seconds. Press Space or gamepad A.\n";
    for (int i = 0; i < 600; ++i)
    {
        input.Update();

        if (input.IsKeyPressed(VK_SPACE))
            std::cout << "Space pressed\n";
        if (input.IsKeyReleased(VK_SPACE))
            std::cout << "Space released\n";
        if (input.IsButtonPressed(GameInputGamepadA))
            std::cout << "Gamepad A pressed\n";

        Sleep(16); // ~60Hz poll, just for this standalone test
    }
    return 0;
}
*/