// Zep (the script editor's widget) without the game: an editor made and a text loaded, edited and read back, with an
// ImGui context and its default font, as the script editor does it on the render thread
#include "imgui/zep.h"

#include <gtest/gtest.h>

TEST(Zep, LoadsEditsAndReadsBackText)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    {
        Zep::ZepEditor_ImGui editor(std::filesystem::temp_directory_path(), Zep::NVec2f(1.f, 1.f), Zep::ZepEditorFlags::DisableThreads);
        editor.InitWithText("main.lua", "local x = 1\nprint(x)\n");
        editor.SetGlobalMode(Zep::ZepMode_Vim::StaticName());		// Only once there is a window: Zep asserts otherwise
        editor.SetGlobalMode(Zep::ZepMode_Standard::StaticName());
        Zep::ZepBuffer* b = editor.GetActiveBuffer();
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(b->GetBufferText(b->Begin(), b->End()), "local x = 1\nprint(x)\n");
        b->SetText("return 2\n");
        EXPECT_EQ(b->GetBufferText(b->Begin(), b->End()), "return 2\n");
    }
    ImGui::DestroyContext();
}
