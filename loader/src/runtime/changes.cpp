#include "runtime/changes.h"

#include "util/glz_compat.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace mirage::changes
{
	namespace
	{
		struct Entry
		{
			MirageHandle object{};
			std::string path, label;		// label: "<object name>.<path>"
			MirageValue old{};
			std::function<bool()> undo;		// A structural change (RecordUndo): put back by this instead of a value
		};
		struct Transaction
		{
			int id = 0;
			std::string name;
			std::vector<Entry> entries;
			bool reverted = false;
		};

		std::mutex g_lock;
		std::map<std::string, std::vector<Transaction>, std::less<>> g_mods;		// By mod id, oldest first
		int g_nextId = 1;
		Writer g_writer;
		thread_local Transaction* t_open = nullptr;		// The open transaction (points into g_mods)
		thread_local std::string t_openMod;
		thread_local bool t_reverting = false;

		Result RevertOne(Transaction& t)
		{
			Result r;
			if (t.reverted) return r;
			t_reverting = true;
			for (auto e = t.entries.rbegin(); e != t.entries.rend(); ++e) {
				if (e->undo) {
					if (e->undo()) ++r.restored;
					else ++r.gone;
					continue;
				}
				MirageObject* o = g_writer.resolve ? g_writer.resolve(&e->object) : nullptr;
				if (o && g_writer.set && g_writer.set(o, e->path.c_str(), &e->old)) ++r.restored;
				else ++r.gone;
			}
			t_reverting = false;
			t.reverted = true;
			return r;
		}
	}

	void SetWriter(const Writer& w) { g_writer = w; }

	int Begin(const std::string& modId, const std::string& name)
	{
		if (t_open) return 0;
		std::lock_guard lock(g_lock);
		auto& list = g_mods[modId];
		Transaction t;
		t.id = g_nextId++;
		t.name = name.empty() ? "transaction " + std::to_string(t.id) : name;
		list.push_back(std::move(t));
		t_open = &list.back();
		t_openMod = modId;
		return t_open->id;
	}

	void End()
	{
		t_open = nullptr;
		t_openMod.clear();
	}

	bool Recording() { return t_open && !t_reverting; }

	void Record(MirageObject* object, const char* path, const MirageValue& old)
	{
		if (!Recording() || !object || !path) return;
		MirageHandle h{};
		h.size = sizeof h;
		if (!g_writer.handle || !g_writer.handle(object, &h)) return;
		std::lock_guard lock(g_lock);
		for (const Entry& e : t_open->entries)
			if (e.object.object == h.object && e.object.slot == h.slot && e.path == path) return;	// Its first old value stays
		Entry e;
		e.object = h;
		e.path = path;
		char name[128]{};
		if (g_writer.name) g_writer.name(object, name, sizeof name);
		e.label = std::string(name) + "." + path;
		e.old = old;
		t_open->entries.push_back(std::move(e));
	}

	bool RecordUndo(const std::string& label, std::function<bool()> undo)
	{
		if (!Recording() || !undo) return false;
		std::lock_guard lock(g_lock);
		Entry e;
		e.label = label;
		e.undo = std::move(undo);
		t_open->entries.push_back(std::move(e));
		return true;
	}

	Result Revert(const std::string& modId, int id)
	{
		Result total;
		std::lock_guard lock(g_lock);
		const auto it = g_mods.find(modId);
		if (it == g_mods.end()) return total;
		for (auto t = it->second.rbegin(); t != it->second.rend(); ++t) {
			if (id != -1 && t->id != id) continue;
			if (&*t == t_open) continue;		// Not the one being recorded now
			const Result r = RevertOne(*t);
			total.restored += r.restored;
			total.gone += r.gone;
		}
		return total;
	}

	Result RevertAndForget(const std::string& modId)
	{
		const Result r = Revert(modId, -1);
		std::lock_guard lock(g_lock);
		if (t_open && t_openMod == modId) End();
		g_mods.erase(modId);
		return r;
	}

	std::vector<Info> List(const std::string& modId)
	{
		std::vector<Info> out;
		std::lock_guard lock(g_lock);
		const auto it = g_mods.find(modId);
		if (it == g_mods.end()) return out;
		for (const Transaction& t : it->second) {
			Info i;
			i.id = t.id;
			i.name = t.name;
			i.reverted = t.reverted;
			for (const Entry& e : t.entries) i.paths.push_back(e.label);
			out.push_back(std::move(i));
		}
		return out;
	}

	namespace detail
	{
		struct ListEntry	// What a mod reads: Info with its paths under "values"
		{
			int id;
			std::string name;
			std::vector<std::string> values;
			bool reverted;
		};
	}

	std::string ListJson(const std::string& modId)
	{
		std::vector<detail::ListEntry> out;
		for (Info& i : List(modId)) out.push_back({i.id, std::move(i.name), std::move(i.paths), i.reverted});
		return jc::dump(out);
	}
}
