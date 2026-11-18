// ImGuiColorTextEdit (the script editor's widget) without the game: a Lua text loaded, edited and read back, a marker
// on a line and the line's text, with an ImGui context and its default font, as the script editor does it on the render
// thread
#include <TextEditor.h>

#include <gtest/gtest.h>

TEST(TextEditor, LoadsEditsAndReadsBackText)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    {
        TextEditor editor;
        editor.SetLanguage(TextEditor::Language::Lua());
        ASSERT_NE(editor.GetLanguage(), nullptr);
        editor.SetText("local x = 1\nprint(x)\n");
        EXPECT_EQ(editor.GetText(), "local x = 1\nprint(x)\n");
        EXPECT_EQ(editor.GetLineCount(), 3u);
        EXPECT_EQ(editor.GetLineText(1), "print(x)");
        editor.AddMarker(1, IM_COL32(255, 0, 0, 255), IM_COL32(80, 0, 0, 255), "error", "error");
        EXPECT_TRUE(editor.HasMarkers());
        editor.SetText("return 2\n");
        EXPECT_EQ(editor.GetText(), "return 2\n");
    }
    ImGui::DestroyContext();
}
