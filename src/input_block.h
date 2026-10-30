#pragma once

// Stops the game from seeing keyboard input while the practice menu is open.
// DMC1 reads the keyboard through DirectInput, which this DLL already sits in front of.
namespace input_block {

// Called with the real IDirectInput8 the game just created.
void OnDirectInputCreated(void* directInput);

// Set every frame by the menu.
void SetBlocked(bool blocked);

}  // namespace input_block
