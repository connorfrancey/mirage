#include "core/module.h"
#include "core/tasks.h"
#include "gui.h"
#include "runtime/mem/heap.h"
#include "runtime/mem/profiler.h"
#include "theme.h"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <new>
#include <string_view>

namespace
{
	using namespace mirage::mem;
	constexpr std::uint64_t kMiB = 1ull << 20;
	constexpr std::uint64_t kLargeRegion = 16 * kMiB;
	constexpr ImU32 kFree = IM_COL32(63, 176, 144, 255), kFragment = IM_COL32(187, 171, 77, 255);
	constexpr ImU32 kReserved = IM_COL32(150, 121, 201, 255), kPrivate = IM_COL32(91, 155, 229, 255);
	constexpr ImU32 kMapped = IM_COL32(227, 146, 83, 255), kImage = IM_COL32(79, 181, 209, 255);
	constexpr ImU32 kUnknown = IM_COL32(74, 80, 92, 255), kHidden = IM_COL32(39, 43, 52, 255);
	constexpr ImVec4 kWarning{1.f, 0.72f, 0.34f, 1.f};

	double MiB(std::uint64_t bytes) { return static_cast<double>(bytes) / kMiB; }
	unsigned long long Hex(std::uint64_t address) { return static_cast<unsigned long long>(address); }

	const char* StateName(std::uint32_t state)
	{
		switch (state) {
		case MEM_FREE: return "Free";
		case MEM_RESERVE: return "Reserved";
		case MEM_COMMIT: return "Committed";
		default: return "Unknown";
		}
	}

	const char* TypeName(std::uint32_t type)
	{
		switch (type) {
		case MEM_PRIVATE: return "Private";
		case MEM_MAPPED: return "Mapped";
		case MEM_IMAGE: return "Image";
		default: return "";
		}
	}

	void FormatProtection(std::uint32_t flags, char (&out)[80])
	{
		const char* access = "other";
		switch (flags & 0xff) {
		case 0: access = "-"; break;
		case PAGE_NOACCESS: access = "No access"; break;
		case PAGE_READONLY: access = "R"; break;
		case PAGE_READWRITE: access = "RW"; break;
		case PAGE_WRITECOPY: access = "Copy-on-write"; break;
		case PAGE_EXECUTE: access = "X"; break;
		case PAGE_EXECUTE_READ: access = "RX"; break;
		case PAGE_EXECUTE_READWRITE: access = "RWX"; break;
		case PAGE_EXECUTE_WRITECOPY: access = "X Copy-on-write"; break;
		}
		if (!flags) {
			std::snprintf(out, sizeof out, "%s", access);
			return;
		}
		std::snprintf(out, sizeof out, "%s%s%s%s (0x%X)", access, flags & PAGE_GUARD ? " Guard" : "",
			flags & PAGE_NOCACHE ? " No cache" : "", flags & PAGE_WRITECOMBINE ? " Write combine" : "",
			static_cast<unsigned>(flags));
	}

	const char* ImageName(const MemoryProfileSnapshot& snapshot, std::size_t index)
	{
		if (index >= snapshot.regionImages.size() || !snapshot.regionImages[index]) return "";
		return snapshot.images[snapshot.regionImages[index] - 1].name.c_str();
	}

	ImU32 RegionColor(const ProcessRegion& region)
	{
		if (region.state == MEM_FREE) return region.Size() < kMiB ? kFragment : kFree;
		ImU32 color = kUnknown;
		if (region.state == MEM_RESERVE) color = kReserved;
		else if (region.type == MEM_IMAGE) color = kImage;
		else if (region.type == MEM_MAPPED) color = kMapped;
		else if (region.type == MEM_PRIVATE) color = kPrivate;
		if (region.Size() >= kLargeRegion) {
			const auto brighten = [](ImU32 channel) { return std::min(channel + 38u, 255u); };
			color = IM_COL32(brighten((color >> IM_COL32_R_SHIFT) & 255), brighten((color >> IM_COL32_G_SHIFT) & 255),
				brighten((color >> IM_COL32_B_SHIFT) & 255), 255);
		}
		return color;
	}

	void ShowRegion(const MemoryProfileSnapshot& snapshot, const ProcessRegion& region)
	{
		char protection[80];
		FormatProtection(region.protection, protection);
		ImGui::Text("0x%08llX - 0x%08llX (end exclusive)", Hex(region.begin), Hex(region.end));
		ImGui::Text("%.2f MiB | %s %s | %s", MiB(region.Size()), StateName(region.state),
			TypeName(region.type), protection);
		if (region.allocationBase) ImGui::Text("Allocation base: 0x%08llX", Hex(region.allocationBase));
		const char* name = ImageName(snapshot, static_cast<std::size_t>(&region - snapshot.regions.data()));
		if (*name) ImGui::TextUnformatted(name);
	}

	bool Contains(std::string_view value, std::string_view query)
	{
		return std::search(value.begin(), value.end(), query.begin(), query.end(), [](unsigned char left, unsigned char right) {
			return std::tolower(left) == std::tolower(right);
		}) != value.end();
	}

	struct RegionFilter
	{
		int state = 0, type = 0, minKiB = 0;
		bool operator==(const RegionFilter&) const = default;

		bool Matches(const ProcessRegion& region) const
		{
			constexpr std::uint32_t states[]{0, MEM_FREE, MEM_RESERVE, MEM_COMMIT};
			constexpr std::uint32_t types[]{0, MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE};
			return (!state || region.state == states[state]) && (!type || region.type == types[type])
				&& region.Size() >= static_cast<std::uint64_t>(minKiB) * 1024;
		}
	};

	struct RegionQuery
	{
		RegionFilter filter;
		std::array<char, 80> text{};
		int column = 0;
		bool descending = false;
		bool operator==(const RegionQuery&) const = default;
	};

	struct RegionRows
	{
		std::shared_ptr<const MemoryProfileSnapshot> snapshot;
		std::vector<std::uint32_t> indices;
	};

	RegionRows IndexRegions(std::shared_ptr<const MemoryProfileSnapshot> snapshot, const RegionQuery& query)
	{
		RegionRows rows{std::move(snapshot), {}};
		const auto& sample = *rows.snapshot;
		rows.indices.reserve(sample.regions.size());
		const std::string_view text(query.text.data());
		for (std::size_t i = 0; i < sample.regions.size(); ++i) {
			if (i % 256 == 0 && tasks::Cancelled()) return {};
			const auto& region = sample.regions[i];
			if (!query.filter.Matches(region)) continue;
			if (!text.empty()) {
				char range[48], protection[80];
				std::snprintf(range, sizeof range, "0x%08llX 0x%08llX", Hex(region.begin), Hex(region.end));
				FormatProtection(region.protection, protection);
				if (!Contains(range, text) && !Contains(StateName(region.state), text)
					&& !Contains(TypeName(region.type), text) && !Contains(protection, text)
					&& !Contains(ImageName(sample, i), text)) continue;
			}
			rows.indices.push_back(static_cast<std::uint32_t>(i));
		}
		const auto sortedBy = [&](auto key) {
			std::ranges::sort(rows.indices, [&](auto a, auto b) {
				const auto ka = key(a), kb = key(b);
				if (ka == kb) return a < b;
				return query.descending ? kb < ka : ka < kb;
			});
		};
		switch (query.column) {
		case 0: if (query.descending) std::ranges::reverse(rows.indices); break;
		case 1: sortedBy([&](auto i) { return sample.regions[i].Size(); }); break;
		case 2: sortedBy([&](auto i) { return std::string_view(StateName(sample.regions[i].state)); }); break;
		case 3: sortedBy([&](auto i) { return std::string_view(TypeName(sample.regions[i].type)); }); break;
		case 4: sortedBy([&](auto i) { return sample.regions[i].protection; }); break;
		case 5: sortedBy([&](auto i) { return std::string_view(ImageName(sample, i)); }); break;
		}
		return rows;
	}

	class MemoryProfilerWindow final : public Module
	{
	public:
		MemoryProfilerWindow() : Module("Memory Profiler", "Debug", "Inspect process memory and contiguous address-space headroom") {}
		void OnEnable() override
		{
			std::lock_guard lock(indexLock_);
			indexEnabled_ = true;
			Profiler().SetInterval(interval_.load());
		}

		void OnDisable() override
		{
			std::lock_guard lock(indexLock_);
			indexEnabled_ = false;
			indexing_.Cancel();
			indexing_ = {};
			indexed_ = {};
			indexRequestedAt_ = 0;
			Profiler().SetInterval(0);
		}

		void OnRender() override
		{
			if (!gui::visible) return;
			theme::Begin();
			ImGui::SetNextWindowSize({840.f, 590.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({530.f, 360.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Memory Profiler", &open)) Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		void Draw()
		{
			int interval = interval_.load();
			ImGui::SetNextItemWidth(220.f);
			if (ImGui::SliderInt("Capture interval", &interval, 1, 60, "%d s")) {
				interval_.store(interval);
				std::lock_guard lock(indexLock_);
				if (indexEnabled_) Profiler().SetInterval(interval);
			}
			const auto snapshot = Profiler().Latest();
			if (Profiler().CaptureFailed()) ImGui::TextColored(kWarning, "A detail capture ran out of memory. Retrying at the next interval.");
			if (!snapshot) {
				ImGui::TextUnformatted("Waiting for the memory monitor.");
				return;
			}
			const auto& space = snapshot->address;
			const double age = static_cast<double>(RealTimeMs() - snapshot->capturedAtMs) / 1000.0;
			ImGui::Text("Sample age %.1f s | Address walk %.2f ms | %zu regions", age, space.walkMs, space.regions);
			if (age > interval + 3.) ImGui::TextColored(kWarning, "The displayed sample is stale.");
			if (!space.complete) ImGui::TextColored(kWarning, "Partial address walk (Windows error %u). Totals cover only the scanned range.", space.queryError);
			if (snapshot->regions.size() < space.regions)
				ImGui::TextColored(kWarning, "Showing the first %zu regions. Totals include all scanned regions.", snapshot->regions.size());

			if (!ImGui::BeginTabBar("Memory views")) return;
			if (ImGui::BeginTabItem("Overview")) { Overview(*snapshot); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Address Space Map")) { Map(*snapshot); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Memory Timeline")) { Timeline(*snapshot); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Region Inspector")) { Inspector(snapshot); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Diagnostics")) { Diagnostics(*snapshot); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}

		static void Overview(const MemoryProfileSnapshot& snapshot)
		{
			const auto& a = snapshot.address;
			const auto& p = snapshot.process;
			const double occupied = a.end > a.begin ? static_cast<double>(a.committed + a.reserved) / (a.end - a.begin) : 0.;
			if (a.complete) {
				char label[80];
				std::snprintf(label, sizeof label, "%.1f%% occupied of %.1f MiB", 100. * occupied, MiB(a.end - a.begin));
				ImGui::ProgressBar(static_cast<float>(occupied), {-FLT_MIN, 0.f}, label);
			}
			ImGui::Text("Committed VA %.1f MiB  (sampled peak %.1f MiB)", MiB(a.committed), MiB(snapshot.peakCommitted));
			ImGui::Text("Reserved VA %.1f MiB  (sampled peak %.1f MiB)", MiB(a.reserved), MiB(snapshot.peakReserved));
			ImGui::Text("Free VA %.1f MiB  |  Largest free block %.1f MiB", MiB(a.free.totalFree), MiB(a.free.largestFree));
			ImGui::Text("Largest aligned span for a new reservation %.1f MiB", MiB(a.largestAlignedFree));
			ImGui::Separator();
			if (p.countersOk) {
				ImGui::Text("Private bytes %.1f MiB  |  Peak process commit %.1f MiB", MiB(p.privateBytes), MiB(p.peakPrivate));
				ImGui::Text("Resident working set %.1f MiB  (OS peak %.1f MiB)", MiB(p.workingSet), MiB(p.peakWorkingSet));
			} else {
				ImGui::TextDisabled("Process memory counters unavailable.");
			}
			if (p.limitsOk) {
				ImGui::Text("Commit headroom %.1f MiB  |  Physical RAM available %.1f MiB",
					MiB(p.commitAvailable), MiB(p.physicalAvailable));
			} else {
				ImGui::TextDisabled("Commit headroom and physical RAM counters unavailable.");
			}
			ImGui::TextColored(snapshot.pressure == Level::Normal ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kWarning,
				"Memory pressure: %s", LevelName(snapshot.pressure));
		}

		static void Diagnostics(const MemoryProfileSnapshot& snapshot)
		{
			const auto& free = snapshot.address.free;
			ImGui::Text("Memory pressure: %s", LevelName(snapshot.pressure));
			if (snapshot.pressureReason[0]) ImGui::TextWrapped("%s", snapshot.pressureReason.data());
			if (snapshot.privateGrowthBytesPerSecond > 0.0)
				ImGui::TextColored(kWarning, "Sustained private-byte growth: %.1f MiB/min", snapshot.privateGrowthBytesPerSecond * 60.0 / kMiB);
			if (free.totalFree) {
				ImGui::Text("Fragmentation index %.1f%%  |  %zu free regions", 100. * free.Index(), free.freeRegions);
				if (free.Index() >= 0.75 && snapshot.address.largestAlignedFree < 128 * kMiB)
					ImGui::TextColored(kWarning, "Free space is spread across small blocks. Large reservations may fail.");
			} else {
				ImGui::TextDisabled("No free blocks in the scanned range.");
			}
			ImGui::Text("Free region counts");
			constexpr const char* names[]{"<64 KiB", "64 KiB to <1 MiB", "1 to <16 MiB", "16 to <128 MiB", "128 MiB and up"};
			for (std::size_t i = 0; i < free.distribution.size(); ++i)
				ImGui::BulletText("%s: %zu", names[i], free.distribution[i]);
			ImGui::Separator();
			ImGui::Text("Mirage operator new failures since load: %llu", Hex(snapshot.allocationFailures));
			if (snapshot.allocationFailures) ImGui::Text("Last requested size: %llu bytes", Hex(snapshot.lastFailedBytes));
			ImGui::TextDisabled("Coverage: Mirage's counted allocator. Engine and external allocators are outside this counter.");
			if (ImGui::CollapsingHeader("Recent memory manager findings", ImGuiTreeNodeFlags_DefaultOpen)) {
				if (!snapshot.findingsCount) ImGui::TextDisabled("No recent findings.");
				for (std::size_t i = 0; i < snapshot.findingsCount; ++i)
					ImGui::TextWrapped("%s: %s", snapshot.findings[i].subject.data(), snapshot.findings[i].text.data());
			}
			if (ImGui::CollapsingHeader("Module attribution")) Attribution(snapshot);
		}

		static void Attribution(const MemoryProfileSnapshot& snapshot)
		{
			ImGui::TextWrapped("Image mappings and Lua heap counters describe different memory. Image mapping totals exclude private allocations made by the module.");
			if (!snapshot.imagesComplete) ImGui::TextColored(kWarning, "Module enumeration was incomplete.");
			if (ImGui::TreeNode("Loaded images")) {
				for (const auto& image : snapshot.images)
					ImGui::BulletText("%s | %.2f MiB committed, %.2f MiB reserved", image.name.c_str(),
						MiB(image.committed), MiB(image.reserved));
				ImGui::TreePop();
			}
			if (ImGui::TreeNode("Lua mods")) {
				if (!snapshot.luaCount) ImGui::TextDisabled("No Lua heap counters available.");
				for (std::size_t i = 0; i < snapshot.luaCount; ++i) {
					const auto& [id, bytes] = snapshot.modLua[i];
					ImGui::BulletText("%s | %.2f MiB", id.data(), MiB(bytes));
				}
				if (snapshot.moreLuaMods) ImGui::TextDisabled("Showing the first 64 reported mods.");
				ImGui::TreePop();
			}
		}

		static void Timeline(const MemoryProfileSnapshot& snapshot)
		{
			if (!snapshot.historyCount) {
				ImGui::TextDisabled("Waiting for a complete address walk.");
				return;
			}
			const auto& first = snapshot.Point(0);
			const auto& last = snapshot.Point(snapshot.historyCount - 1);
			ImGui::Text("%zu complete captures over %.1f s | Oldest to newest | Independent scales",
				snapshot.historyCount, last.seconds - first.seconds);
			using Series = std::uint64_t ProfilePoint::*;
			const auto draw = [&](const char* title, Series series) {
				struct Plot { const MemoryProfileSnapshot& snapshot; Series series; } plot{snapshot, series};
				ImGui::Text("%s  %.1f MiB", title, MiB(last.*series));
				ImGui::PushID(title);
				ImGui::PlotLines("##history", [](void* data, int i) {
					const auto& p = *static_cast<const Plot*>(data);
					return static_cast<float>(MiB(p.snapshot.Point(static_cast<std::size_t>(i)).*p.series));
				}, &plot, static_cast<int>(snapshot.historyCount), 0, nullptr, FLT_MAX, FLT_MAX,
					{-FLT_MIN, 66.f});
				ImGui::PopID();
			};
			draw("Committed VA", &ProfilePoint::committed);
			draw("Reserved VA", &ProfilePoint::reserved);
			draw("Free VA", &ProfilePoint::free);
			draw("Largest free block", &ProfilePoint::largestFree);
		}

		void Filters()
		{
			const char* states[]{"All states", "Free", "Reserved", "Committed"};
			const char* types[]{"All types", "Private", "Mapped", "Image"};
			ImGui::SetNextItemWidth(110.f);
			ImGui::Combo("##state-filter", &query_.filter.state, states, IM_ARRAYSIZE(states));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.f);
			ImGui::Combo("##type-filter", &query_.filter.type, types, IM_ARRAYSIZE(types));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.f);
			ImGui::DragInt("Min KiB", &query_.filter.minKiB, 16.f, 0, 4 * 1024 * 1024, "%d", ImGuiSliderFlags_AlwaysClamp);
		}

		void Map(const MemoryProfileSnapshot& snapshot)
		{
			Filters();
			const double low = static_cast<double>(snapshot.address.begin);
			const double high = static_cast<double>(snapshot.address.end);
			const double full = high - low;
			if (full <= 0.) return;
			if (ImGui::Button("Fit address space") || span_ <= 0.) { view_ = low; span_ = full; }
			ImGui::SameLine();
			ImGui::TextDisabled("Wheel to zoom, drag to pan, click to select");
			ImGui::BeginChild("##map-view", {0.f, 108.f}, ImGuiChildFlags_None,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			const float width = std::max(ImGui::GetContentRegionAvail().x, 1.f);
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const ImVec2 bottom{origin.x + width, origin.y + 76.f};
			const bool clicked = ImGui::InvisibleButton("##address-map", {width, 76.f});
			const bool hovered = ImGui::IsItemHovered();
			if (ImGui::IsItemActivated()) dragged_ = false;
			const auto& io = ImGui::GetIO();
			const double cursor = std::clamp(static_cast<double>((io.MousePos.x - origin.x) / width), 0., 0.999999999);
			span_ = std::min(span_, full);
			if (hovered && io.MouseWheel != 0.f) {
				const double zoom = std::clamp(span_ * std::pow(0.75, io.MouseWheel), std::min(4096., full), full);
				view_ += cursor * (span_ - zoom);
				span_ = zoom;
			}
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
				view_ -= io.MouseDelta.x * span_ / width;
				dragged_ = true;
			}
			view_ = std::clamp(view_, low, high - span_);
			const auto point = static_cast<std::uint64_t>(view_ + cursor * span_);
			const auto* hit = hovered ? FindProcessRegion(snapshot.regions, point) : nullptr;
			if (clicked && hit && query_.filter.Matches(*hit) && !dragged_) selected_ = point;
			DrawMap(snapshot, origin, bottom);
			ImGui::Text("0x%08llX - 0x%08llX | %.2f MiB visible", Hex(static_cast<std::uint64_t>(view_)),
				Hex(static_cast<std::uint64_t>(view_ + span_)), span_ / kMiB);
			if (hovered && ImGui::BeginTooltip()) {
				if (hit) ShowRegion(snapshot, *hit);
				else ImGui::Text("0x%08llX | No region data", Hex(point));
				ImGui::EndTooltip();
			}
			ImGui::EndChild();
			Legend();
			ShowSelection(snapshot);
		}

		struct MapView
		{
			double begin = 0., span = 0.;
			std::uint64_t sample = 0;
			int bins = 0;
			RegionFilter filter;
			bool operator==(const MapView&) const = default;
		};

		void DrawMap(const MemoryProfileSnapshot& snapshot, ImVec2 origin, ImVec2 bottom)
		{
			auto* draw = ImGui::GetWindowDrawList();
			const float width = bottom.x - origin.x;
			const int bins = static_cast<int>(std::clamp(width, 1.f, static_cast<float>(mapColors_.size())));
			const MapView view{view_, span_, snapshot.capturedAtMs, bins, query_.filter};
			if (mapView_ != view) {
				const double bytesPerBin = span_ / bins;
				for (int i = 0; i < bins; ++i) {
					const auto point = static_cast<std::uint64_t>(view_ + (i + 0.5) * bytesPerBin);
					const auto* region = FindProcessRegion(snapshot.regions, point);
					mapColors_[i] = region ? (query_.filter.Matches(*region) ? RegionColor(*region) : kHidden) : kUnknown;
				}
				mapView_ = view;
			}
			const float pixelsPerBin = width / bins;
			ImU32 previous = mapColors_[0];
			int first = 0;
			const auto line = [&](int last, ImU32 color) {
				if (last > first) draw->AddRectFilled({origin.x + first * pixelsPerBin, origin.y},
					{origin.x + last * pixelsPerBin, bottom.y}, color);
			};
			for (int i = 1; i < bins; ++i) {
				const ImU32 color = mapColors_[i];
				if (color == previous) continue;
				line(i, previous);
				first = i;
				previous = color;
			}
			line(bins, previous);
			if (selected_ >= view_ && selected_ < view_ + span_) {
				const float x = origin.x + static_cast<float>(((selected_ - view_) / span_) * width);
				draw->AddLine({x, origin.y}, {x, bottom.y}, IM_COL32_WHITE, 2.f);
			}
		}

		static void Legend()
		{
			const auto item = [](const char* label, ImU32 color) {
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s", label);
			};
			item("Free", kFree); ImGui::SameLine();
			item("Free <1 MiB", kFragment); ImGui::SameLine();
			item("Reserved", kReserved); ImGui::SameLine();
			item("Private", kPrivate); ImGui::SameLine();
			item("Mapped", kMapped); ImGui::SameLine();
			item("Image", kImage); ImGui::SameLine();
			item("No data", kUnknown);
			ImGui::TextDisabled("Bright shades mark occupied regions of 16 MiB or more. Zoom in to resolve small regions.");
		}

		void ShowSelection(const MemoryProfileSnapshot& snapshot) const
		{
			if (!selected_) return;
			ImGui::SeparatorText("Selected address");
			if (const auto* region = FindProcessRegion(snapshot.regions, selected_)) ShowRegion(snapshot, *region);
			else ImGui::TextDisabled("0x%08llX | No region data", Hex(selected_));
		}

		void Inspector(const std::shared_ptr<const MemoryProfileSnapshot>& latest)
		{
			Filters();
			ImGui::InputTextWithHint("##region-search", "Address, state, type, protection or module", query_.text.data(), query_.text.size());
			ShowSelection(*latest);
			constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
				| ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
			if (!ImGui::BeginTable("##regions", 6, flags, {0.f, -ImGui::GetTextLineHeightWithSpacing()})) return;
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_DefaultSort);
			ImGui::TableSetupColumn("Size");
			ImGui::TableSetupColumn("State");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Protection");
			ImGui::TableSetupColumn("Module");
			ImGui::TableHeadersRow();
			if (auto* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount) {
				query_.column = sort->Specs[0].ColumnIndex;
				query_.descending = sort->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				sort->SpecsDirty = false;
			}
			bool unavailable = false;
			auto result = GetRegionRows(latest, unavailable);
			const auto* rows = result.Result();
			if (!rows || !rows->snapshot) {
				ImGui::EndTable();
				ImGui::TextDisabled("%s", unavailable ? "Region index unavailable. Retrying on the next capture." : "Building region index...");
				return;
			}
			const auto& snapshot = *rows->snapshot;

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(rows->indices.size()));
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const auto index = rows->indices[i];
					const auto& region = snapshot.regions[index];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					char address[32], protection[80];
					std::snprintf(address, sizeof address, "0x%08llX", Hex(region.begin));
					FormatProtection(region.protection, protection);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(address, selected_ >= region.begin && selected_ < region.end, ImGuiSelectableFlags_SpanAllColumns))
						selected_ = region.begin;
					ImGui::PopID();
					if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) { ShowRegion(snapshot, region); ImGui::EndTooltip(); }
					ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f MiB", MiB(region.Size()));
					ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(StateName(region.state));
					ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(TypeName(region.type));
					ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(protection);
					ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(ImageName(snapshot, index));
				}
			}
			ImGui::EndTable();
			ImGui::Text("%zu matching regions%s", rows->indices.size(),
				snapshot.capturedAtMs == latest->capturedAtMs ? "" : " (previous capture)");
		}

		tasks::Task<RegionRows> GetRegionRows(const std::shared_ptr<const MemoryProfileSnapshot>& snapshot, bool& unavailable)
		{
			std::lock_guard lock(indexLock_);
			if (!indexEnabled_) return {};
			if (indexing_.Ok()) indexed_ = indexing_;
			if (indexRequestedAt_ && !indexing_.Ready()) return indexedQuery_ == query_ ? indexed_ : tasks::Task<RegionRows>{};
			if ((indexRequestedAt_ != snapshot->capturedAtMs || indexedQuery_ != query_) && tasks::Running()) {
				if (indexedQuery_ != query_) indexed_ = {};
				indexedQuery_ = query_;
				indexRequestedAt_ = snapshot->capturedAtMs;
				try {
					indexing_ = tasks::Run([snapshot, query = query_] { return IndexRegions(snapshot, query); },
						{.priority = tasks::Priority::Low, .name = "Memory region index"});
				} catch (const std::bad_alloc&) {
					indexing_ = {};
				}
			}
			unavailable = indexing_.GetStatus() == tasks::Status::Failed
				|| (indexRequestedAt_ && indexing_.GetStatus() == tasks::Status::Cancelled);
			return indexed_;
		}

		std::atomic<int> interval_{1};
		RegionQuery query_;
		double view_ = 0., span_ = 0.;
		MapView mapView_;
		std::array<ImU32, 768> mapColors_{};
		std::uint64_t selected_ = 0;
		bool dragged_ = false;
		std::mutex indexLock_;
		bool indexEnabled_ = false;
		tasks::Task<RegionRows> indexing_, indexed_;
		RegionQuery indexedQuery_;
		std::uint64_t indexRequestedAt_ = 0;
	};

	MEM_MODULE(MemoryProfilerWindow)
}
