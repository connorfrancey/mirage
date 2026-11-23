#pragma once

namespace gui {
    inline bool visible = true;

    void ApplyStyle();           // call once after ImGui::CreateContext()
    void Render();               // call between ImGui::NewFrame() and ImGui::Render()
    bool CaptureKey(int vk);     // true if the key was consumed by "press a key to bind"

    // The self-test selects a category until cleared (""). The user's selection is preserved. Any thread.
    void OpenColumnForTest(const char* tab);
}
