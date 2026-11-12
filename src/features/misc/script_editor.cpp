// The script editor: write script mods in the game. A list of the script mods (C# scripts and Lua; the first-party
// ones are written again at every start, so they aren't offered), a tab per file, the code, and its diagnostics: the
// file is checked a moment after typing stops (on a worker, unsaved text included), a click on a diagnostic goes to
// its line. Ctrl+S saves; the mod's hot reload loads what was saved. "New mod" makes one from a template in the build's
// scripting language (runtime/mod_dotnet.h) and loads it at once (mirage::mods::LoadNew).

#include "features/misc/script_editor.h"

#include "core/module.h"
#include "core/tasks.h"
#include "font.h"
#include "game/game.h"
#include "gui.h"
#include "paths.h"
#include "runtime/firstparty.h"
#include "runtime/mod_dotnet.h"
#include "runtime/mods.h"
#include "theme.h"

#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <lauxlib.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	std::atomic<std::uint64_t> g_frames{0};

	std::string U8(const fs::path& p)
	{
		const auto u = p.u8string();
		return std::string(u.begin(), u.end());
	}

	bool IsSource(const fs::path& p)
	{
		const auto ext = p.extension();
		return ext == ".cs" || ext == ".lua";
	}

	// The source files of a mod: .cs and .lua, build output and hot reload copies left out
	std::vector<fs::path> SourcesIn(const fs::path& dir)
	{
		std::vector<fs::path> out;
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			const auto name = it->path().filename();
			if (it->is_directory()) {
				if (name == "obj" || name == "bin" || name == ".mirage") it.disable_recursion_pending();
				continue;
			}
			if (IsSource(it->path())) out.push_back(it->path());
		}
		std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
			const auto rank = [](const fs::path& p) { return p.stem() == "main" ? 0 : 1; };	// main.* first
			return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
		});
		return out;
	}

	// The first-party mods' ids (from the files the DLL carries)
	const std::set<std::string>& FirstParty()
	{
		static const std::set<std::string> ids = [] {
			std::set<std::string> s;
			for (const auto& f : mirage::firstparty::Files()) {
				const std::string path = f.path;
				s.insert(path.substr(0, path.find('/')));
			}
			return s;
		}();
		return ids;
	}

	// A file as the editor holds it: '\n' only; `crlf` remembers how to write it back
	struct Doc
	{
		fs::path path;
		std::string text, saved;
		bool crlf = false;
		fs::file_time_type stamp{};

		bool Dirty() const { return text != saved; }
		bool Load()
		{
			std::ifstream in(path, std::ios::binary);
			if (!in) return false;
			std::ostringstream ss;
			ss << in.rdbuf();
			std::string raw = ss.str();
			crlf = raw.find("\r\n") != std::string::npos;
			std::erase(raw, '\r');
			text = saved = raw;
			std::error_code ec;
			stamp = fs::last_write_time(path, ec);
			return true;
		}
		bool Save()
		{
			std::string out;
			out.reserve(text.size() + text.size() / 16);
			for (char c : text) {
				if (c == '\n' && crlf) out += '\r';
				out += c;
			}
			{
				std::ofstream f(path, std::ios::binary | std::ios::trunc);
				if (!f || !(f << out)) return false;
			}
			saved = text;
			std::error_code ec;
			stamp = fs::last_write_time(path, ec);
			return true;
		}
	};

	// Byte offset of 1-based (line, column) in `text`
	int OffsetOf(const std::string& text, int line, int column)
	{
		int at = 0;
		for (int l = 1; l < line && at < static_cast<int>(text.size()); ++at)
			if (text[at] == '\n') ++l;
		const int end = static_cast<int>(text.find('\n', at) == std::string::npos ? text.size() : text.find('\n', at));
		return std::min(at + std::max(column - 1, 0), end);
	}

	class ScriptEditor final : public Module
	{
	public:
		ScriptEditor() : Module("Script Editor", "Misc", "Write script mods in the game: checked as you type, saved to reload", 2) {}

		void OnRender() override
		{
			if (!gui::visible) return;		// A window to work in: only with the menu open
			++g_frames;
			Poll();
			theme::Begin();
			ImGui::SetNextWindowPos({260.f, 90.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({900.f, 600.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({560.f, 320.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Script Editor", &open, ImGuiWindowFlags_NoCollapse)) {
				const auto mods = mirage::mods::List();
				ImGui::BeginChild("##mods", {210.f, 0.f}, ImGuiChildFlags_Borders);
				DrawModList(mods);
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("##editor", {0.f, 0.f}, ImGuiChildFlags_None);
				const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == selected_; });
				if (it != mods.end()) DrawEditor(*it);
				else ImGui::TextDisabled("Pick a script mod on the left, or make a new one.");
				ImGui::EndChild();
			}
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		// ---- The mod list and "New mod" ------------------------------------------------------------------------------
		void DrawModList(const std::vector<mirage::mods::Info>& mods)
		{
			ImGui::TextDisabled("New %s mod", mirage::dotnet::ScriptLanguage());
			ImGui::SetNextItemWidth(-FLT_MIN);
			const bool enter = ImGui::InputTextWithHint("##newid", "id: my-mod", newId_, sizeof newId_, ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::Button("Create", {-FLT_MIN, 0.f}) || enter) Create();
			{
				std::lock_guard l(statusLock_);
				if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
			}
			ImGui::Separator();
			if (!pendingSelect_.empty())		// A mod just made: open it once the runtime lists it
				for (const auto& m : mods)
					if (m.id == pendingSelect_) {
						pendingSelect_.clear();
						Open(m);
						break;
					}
			int shown = 0;
			for (const auto& m : mods) {
				if (!m.script || FirstParty().count(m.id)) continue;
				++shown;
				ImGui::PushID(m.id.c_str());
				const ImVec4 color = m.loaded ? ImVec4{0.4f, 0.85f, 0.45f, 1.f} : ImVec4{0.95f, 0.45f, 0.35f, 1.f};
				const ImVec2 at = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddCircleFilled({at.x + 6.f, at.y + ImGui::GetTextLineHeight() * 0.5f + 1.f}, 4.f,
															ImGui::GetColorU32(color));
				ImGui::SetCursorScreenPos({at.x + 16.f, at.y});
				if (ImGui::Selectable(m.id.c_str(), selected_ == m.id)) Open(m);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", m.kind.c_str(), m.loaded ? "loaded" : m.problem.c_str());
				ImGui::PopID();
			}
			if (shown == 0) ImGui::TextDisabled("No script mods yet");
		}

		void Create()
		{
			const std::string id = newId_;
			const fs::path modsDir = paths::Dir() / "mods";
			const std::string problem = script_editor::CreateModFiles(modsDir, id);
			std::lock_guard l(statusLock_);
			if (!problem.empty()) {
				status_ = problem;
				return;
			}
			status_ = "Loading " + id + "...";
			newId_[0] = 0;
			pendingSelect_ = id;
			game::RunOnGameThread([this, dir = modsDir / id, id] {
				const std::string why = mirage::mods::LoadNew(dir);
				std::lock_guard l2(statusLock_);
				status_ = why.empty() ? id + " made and loaded" : id + " made, but: " + why;
			});
		}

		void Open(const mirage::mods::Info& m)
		{
			selected_ = m.id;
			docs_.clear();
			current_ = 0;
			check_ = {};
			for (const auto& p : SourcesIn(m.dir)) {
				Doc d;
				d.path = p;
				if (d.Load()) docs_.push_back(std::move(d));
			}
			folder_ = m.dir;
			checkDue_ = !docs_.empty();
			dueAt_ = Clock::now();
		}

		// ---- The editor ---------------------------------------------------------------------------------------------
		void DrawEditor(const mirage::mods::Info& m)
		{
			ImGui::TextUnformatted(m.id.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("(%s)", m.kind.c_str());
			ImGui::SameLine();
			if (m.loaded) ImGui::TextColored({0.4f, 0.85f, 0.45f, 1.f}, "loaded%s", m.reloads ? (", reloaded " + std::to_string(m.reloads) + "x").c_str() : "");
			else ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.f}, "not loaded");
			if (!m.loaded && !m.problem.empty()) {
				const std::string first = m.problem.substr(0, m.problem.find('\n'));
				ImGui::TextWrapped("%s", first.c_str());
			}
			if (docs_.empty()) {
				ImGui::TextDisabled("No .cs or .lua files in %s", U8(m.dir).c_str());
				return;
			}

			// Toolbar
			Doc& doc = docs_[current_];
			ImGui::BeginDisabled(!doc.Dirty());
			if (ImGui::Button("Save")) SaveCurrent();
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+S. The mod reloads with it (hot reload).");
			ImGui::SameLine();
			if (ImGui::Button("Check")) ScheduleCheck(0.0);
			ImGui::SameLine();
			if (ImGui::Button("Reload mod")) {
				const std::string id = m.id;
				game::RunOnGameThread([this, id] {
					const std::string why = mirage::mods::Reload(id);
					std::lock_guard l(statusLock_);
					status_ = why.empty() ? id + " reloaded" : id + ": " + why;
				});
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!doc.Dirty());
			if (ImGui::Button("Revert")) {
				doc.Load();
				ScheduleCheck(0.0);
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Folder")) ShellExecuteW(nullptr, L"open", m.dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);

			// A tab per file (a '*' when it has unsaved changes)
			if (ImGui::BeginTabBar("##files", ImGuiTabBarFlags_FittingPolicyScroll)) {
				for (std::size_t i = 0; i < docs_.size(); ++i) {
					const std::string label = U8(docs_[i].path.lexically_relative(m.dir)) + (docs_[i].Dirty() ? " *" : "") + "###f" + std::to_string(i);
					const ImGuiTabItemFlags flags = jumpTab_ == static_cast<int>(i) ? ImGuiTabItemFlags_SetSelected : 0;
					if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
						if (current_ != i) {
							current_ = i;
							ScheduleCheck(0.0);
						}
						ImGui::EndTabItem();
					}
				}
				jumpTab_ = -1;
				ImGui::EndTabBar();
			}

			// The code
			Doc& d = docs_[current_];
			const float diagHeight = 130.f;
			const ImVec2 size{-FLT_MIN, std::max(120.f, ImGui::GetContentRegionAvail().y - diagHeight - ImGui::GetFrameHeightWithSpacing())};
			if (focusEditor_) {
				ImGui::SetKeyboardFocusHere();
				focusEditor_ = false;
			}
			editing_ = &d.text;
			ImFont* mono = font::Mono();
			if (mono) ImGui::PushFont(mono, 0.f);	// 0: the current size
			const bool edited = ImGui::InputTextMultiline("##code", d.text.data(), d.text.capacity() + 1, size,
				ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways,
				&ScriptEditor::Callback, this);
			if (mono) ImGui::PopFont();
			editing_ = nullptr;
			if (edited) ScheduleCheck(0.6);
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				SaveCurrent();

			// Status line
			ImGui::TextDisabled("Ln %d, Col %d   %s", cursorLine_, cursorColumn_, d.Dirty() ? "unsaved" : "saved");
			ImGui::SameLine();
			if (check_.Ready() || !checking_) {
				const int errors = last_.Errors();
				const int warnings = static_cast<int>(last_.diagnostics.size()) - errors;
				if (!last_.ran) ImGui::TextDisabled("  %s", last_.note.c_str());
				else if (errors) ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.f}, "  %d error(s), %d warning(s)  (%s)", errors, warnings, last_.note.c_str());
				else ImGui::TextColored({0.4f, 0.85f, 0.45f, 1.f}, "  compiles%s  (%s)", warnings ? (", " + std::to_string(warnings) + " warning(s)").c_str() : "", last_.note.c_str());
			} else {
				ImGui::TextDisabled("  checking...");
			}

			// Diagnostics: a click goes to the place
			ImGui::BeginChild("##diagnostics", {0.f, 0.f}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
			for (std::size_t i = 0; i < last_.diagnostics.size(); ++i) {
				const auto& g = last_.diagnostics[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::PushStyleColor(ImGuiCol_Text, g.error ? ImVec4{0.95f, 0.45f, 0.35f, 1.f} : ImVec4{0.95f, 0.8f, 0.3f, 1.f});
				if (ImGui::Selectable(g.text.c_str()) && g.line > 0) GoTo(g);
				ImGui::PopStyleColor();
				ImGui::PopID();
			}
			ImGui::EndChild();
		}

		static int Callback(ImGuiInputTextCallbackData* data)
		{
			auto* self = static_cast<ScriptEditor*>(data->UserData);
			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize && self->editing_) {
				self->editing_->resize(static_cast<std::size_t>(data->BufTextLen));
				data->Buf = self->editing_->data();
			} else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
				if (self->jumpTo_ >= 0) {
					data->CursorPos = data->SelectionStart = data->SelectionEnd = std::min(self->jumpTo_, data->BufTextLen);
					self->jumpTo_ = -1;
				}
				int line = 1, column = 1;
				for (int i = 0; i < data->CursorPos && i < data->BufTextLen; ++i) {
					if (data->Buf[i] == '\n') {
						++line;
						column = 1;
					} else {
						++column;
					}
				}
				self->cursorLine_ = line;
				self->cursorColumn_ = column;
			}
			return 0;
		}

		void GoTo(const script_editor::Diagnostic& g)
		{
			for (std::size_t i = 0; i < docs_.size(); ++i)
				if (U8(docs_[i].path.filename()) == g.file) {
					if (i != current_) {
						current_ = i;
						jumpTab_ = static_cast<int>(i);
					}
					jumpTo_ = OffsetOf(docs_[i].text, g.line, std::max(g.column, 1));
					focusEditor_ = true;
					return;
				}
		}

		void SaveCurrent()
		{
			Doc& d = docs_[current_];
			if (!d.Dirty()) return;
			std::lock_guard l(statusLock_);
			if (d.Save()) status_ = "Saved " + U8(d.path.filename()) + ": reloading";
			else status_ = "Couldn't write " + U8(d.path);
			ScheduleCheck(0.0);
		}

		// ---- Checking -----------------------------------------------------------------------------------------------
		using Clock = std::chrono::steady_clock;

		void ScheduleCheck(double seconds)
		{
			checkDue_ = true;
			dueAt_ = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
		}

		void Poll()
		{
			if (checking_ && check_.Ready()) {
				checking_ = false;
				if (const auto* r = check_.Result()) last_ = *r;
				else last_ = {false, {}, "the check failed: " + check_.Error()};
			}
			if (!checkDue_ || checking_ || Clock::now() < dueAt_ || docs_.empty() || current_ >= docs_.size()) return;
			checkDue_ = false;
			checking_ = true;
			check_ = tasks::Run([folder = folder_, file = docs_[current_].path, text = docs_[current_].text] {
				return script_editor::Check(folder, file, text);
			}, {.priority = tasks::Priority::High, .name = "Script check"});
		}

		std::string selected_, pendingSelect_;
		fs::path folder_;
		std::vector<Doc> docs_;
		std::size_t current_ = 0;
		std::string* editing_ = nullptr;
		int jumpTo_ = -1, jumpTab_ = -1;
		bool focusEditor_ = false;
		int cursorLine_ = 1, cursorColumn_ = 1;
		char newId_[64] = {};

		std::mutex statusLock_;		// status_ is written from the game thread (a load's result)
		std::string status_;

		tasks::Task<script_editor::CheckResult> check_;
		script_editor::CheckResult last_;
		bool checking_ = false, checkDue_ = false;
		Clock::time_point dueAt_{};
	};
	MEM_MODULE(ScriptEditor)
}

// ---- The parts without a window -----------------------------------------------------------------------------------------
std::uint64_t script_editor::Frames() { return g_frames.load(); }

int script_editor::CheckResult::Errors() const
{
	return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.error; }));
}

std::vector<script_editor::Diagnostic> script_editor::ParseDiagnostics(const std::string& output)
{
	std::vector<Diagnostic> out;
	std::istringstream in(output);
	std::string line;
	const auto number = [](const std::string& s, std::size_t& at) {
		int v = 0;
		const std::size_t from = at;
		while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) v = v * 10 + (s[at++] - '0');
		return at > from ? v : -1;
	};
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		Diagnostic d;
		d.text = line;
		// C#: file(line,col): error CSxxxx: message
		const std::size_t paren = line.find('(');
		if (paren != std::string::npos && paren > 0) {
			std::size_t at = paren + 1;
			const int l = number(line, at);
			if (l > 0 && at < line.size() && line[at] == ',') {
				++at;
				const int c = number(line, at);
				if (c > 0 && line.compare(at, 3, "): ") == 0) {
					d.file = line.substr(0, paren);
					d.line = l;
					d.column = c;
					d.error = line.compare(at + 3, 5, "error") == 0;
					out.push_back(std::move(d));
					continue;
				}
			}
		}
		// Lua: file:line: message
		const std::size_t colon = line.find(':');
		if (colon != std::string::npos && colon > 0) {
			std::size_t at = colon + 1;
			const int l = number(line, at);
			if (l > 0 && at < line.size() && line[at] == ':') {
				d.file = line.substr(0, colon);
				d.line = l;
				d.column = 1;
				d.error = true;
				out.push_back(std::move(d));
				continue;
			}
		}
		if (line.front() == '(' && line.back() == ')') continue;	// The C# check's "(13 ms)"
		out.push_back(std::move(d));	// Something else the compiler said: shown, not a place
	}
	return out;
}

script_editor::CheckResult script_editor::Check(const fs::path& folder, const fs::path& file, const std::string& text)
{
	CheckResult r;
	const auto t0 = std::chrono::steady_clock::now();
	if (file.extension() == ".cs") {
		if (!mirage::dotnet::kEnabled) {
			r.note = "this build has no C# (its scripting language is Lua)";
			return r;
		}
		const auto c = mirage::dotnet::CheckScripts(folder, file, text);
		if (c.errors < 0) {
			r.note = c.output;
			return r;
		}
		r.ran = true;
		r.diagnostics = ParseDiagnostics(c.output);
		const std::size_t ms = c.output.rfind('(');
		r.note = ms != std::string::npos ? c.output.substr(ms + 1, c.output.find(')', ms) - ms - 1) : std::string();
		return r;
	}
	if (file.extension() == ".lua") {		// Syntax only: the file is compiled, never run
		lua_State* L = luaL_newstate();
		if (!L) {
			r.note = "no memory for a Lua state";
			return r;
		}
		const std::string chunk = "=" + U8(file.filename());
		if (luaL_loadbufferx(L, text.data(), text.size(), chunk.c_str(), "t") != LUA_OK) {
			const char* message = lua_tostring(L, -1);
			r.diagnostics = ParseDiagnostics(message ? message : "?");
		}
		lua_close(L);
		r.ran = true;
		r.note = std::to_string(static_cast<int>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count())) + " ms";
		return r;
	}
	r.note = "not a .cs or .lua file";
	return r;
}

std::string script_editor::Template(const std::string& id)
{
	if (!mirage::dotnet::kEnabled)
		return "-- " + id + ": a Lua script mod. Save (Ctrl+S) and it reloads in the game.\n"
			   "\n"
			   "mirage.module {\n"
			   "    name = \"" + id + "\",\n"
			   "    tab = \"Mods\",\n"
			   "    description = \"My Lua script\",\n"
			   "\n"
			   "    on_tick = function()\n"
			   "    end,\n"
			   "\n"
			   "    on_render = function()\n"
			   "        mirage.draw.text(20, 60, mirage.rgba(255, 255, 255), \"Hello from " + id + "\")\n"
			   "    end,\n"
			   "}\n";
	// C#: the class names from the id ("my-mod" -> MyMod)
	std::string name;
	bool upper = true;
	for (char c : id) {
		if (!std::isalnum(static_cast<unsigned char>(c))) {
			upper = true;
			continue;
		}
		name += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
		upper = false;
	}
	if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front()))) name = "Mod" + name;
	return "// " + id + ": a C# script mod. Save (Ctrl+S) and it reloads in the game.\n"
		   "// System, System.Linq, System.Collections.Generic and Mirage are imported already.\n"
		   "\n"
		   "public sealed class " + name + " : Mod\n"
		   "{\n"
		   "    public override void OnLoad() => new " + name + "Module(this);\n"
		   "}\n"
		   "\n"
		   "public sealed class " + name + "Module : Module\n"
		   "{\n"
		   "    public " + name + "Module(Mod mod) : base(mod, \"" + id + "\", \"Mods\", \"My C# script\") { }\n"
		   "\n"
		   "    protected override void OnTick()             // Game thread, every tick while on\n"
		   "    {\n"
		   "        var pawn = GameObject.Player;\n"
		   "        if (!pawn.IsValid) return;\n"
		   "    }\n"
		   "\n"
		   "    protected override void OnRender() =>        // Render thread\n"
		   "        Draw.Text(20, 60, 0xFFFFFFFF, \"Hello from " + id + "\");\n"
		   "}\n";
}

std::string script_editor::CreateModFiles(const fs::path& modsDir, const std::string& id)
{
	if (id.empty() || id.size() > 48) return "The id needs 1 to 48 characters";
	for (char c : id)
		if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
			return "The id may only have a-z, 0-9, - and _";
	const fs::path dir = modsDir / id;
	std::error_code ec;
	if (fs::exists(dir, ec)) return "There is a folder " + id + " already";
	if (!fs::create_directories(dir, ec)) return "Couldn't make " + U8(dir) + ": " + ec.message();
	const std::string language = mirage::dotnet::ScriptLanguage();
	{
		std::ofstream toml(dir / "mod.toml", std::ios::binary);
		toml << "# Made in the game's script editor\n"
			 << "id = \"" << id << "\"\n"
			 << "name = \"" << id << "\"\n"
			 << "version = \"0.1.0\"\n"
			 << "description = \"A " << language << " script mod\"\n"
			 << "mirage = \"^0.1\"\n"
			 << "hot_reload = true\n";
		if (!toml) return "Couldn't write mod.toml";
	}
	std::ofstream main(dir / mirage::dotnet::ScriptFile(), std::ios::binary);
	if (!(main << Template(id))) return std::string("Couldn't write ") + mirage::dotnet::ScriptFile();
	spdlog::info("Script editor: made {} ({})", U8(dir), language);
	return {};
}
