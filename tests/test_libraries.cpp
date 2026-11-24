// Libraries Mirage builds with, used the way Mirage uses them: glaze (JSON from a struct's own members, C++23) and
// ImReflect (ImGui inputs drawn for a struct), in a headless ImGui frame

#include <glaze/glaze.hpp>
#include <imgui.h>
#include <ImReflect.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace library_test		// Named: glaze's reflection needs a type with external linkage (MSVC)
{
	struct Settings
	{
		int volume = 50;
		float sensitivity = 1.0f;
		bool fullscreen = false;
		std::string name = "Faith";
		std::vector<int> keys{1, 2, 3};
	};
}
using library_test::Settings;
IMGUI_REFLECT(Settings, volume, sensitivity, fullscreen, name)

TEST(Libraries, GlazeRoundTrip)
{
	const Settings in{70, 2.5f, true, "Kate", {4, 5}};
	std::string json;
	ASSERT_FALSE(glz::write_json(in, json));
	EXPECT_EQ(json, R"({"volume":70,"sensitivity":2.5,"fullscreen":true,"name":"Kate","keys":[4,5]})");
	Settings out;
	ASSERT_FALSE(glz::read_json(out, json));
	EXPECT_EQ(out.volume, 70);
	EXPECT_EQ(out.name, "Kate");
	EXPECT_EQ(out.keys, (std::vector<int>{4, 5}));
	EXPECT_TRUE(glz::read_json(out, std::string("{\"volume\":")));		// Cut short: an error
}

TEST(Libraries, ImReflectDrawsAStruct)
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = {800, 600};
	unsigned char* pixels = nullptr;
	int w = 0, h = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
	ImGui::NewFrame();
	Settings s;
	ImGui::Begin("Settings");
	ImReflect::Input("Settings", s);
	const ImGuiID volume = ImGui::GetID("Settings");
	ImGui::End();
	ImGui::EndFrame();
	EXPECT_NE(volume, 0u);
	EXPECT_EQ(s.volume, 50);		// Drawn, not changed
	ImGui::DestroyContext();
}
