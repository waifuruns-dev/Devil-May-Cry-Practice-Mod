#pragma once

// Practice loader menu: Category -> Mission -> Load (writes the practice slot of dmc1.sav).
namespace menu {

void Init();
void Update(bool gameFocused);  // reads keys; call every frame
void Draw();                    // call between ImGui::NewFrame and Render
bool IsOpen();

}  // namespace menu
