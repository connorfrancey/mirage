#include "runtime/mem/analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mirage::mem
{
	namespace
	{
		std::string Mb(double bytes)
		{
			char buf[32];
			std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
			return buf;
		}
	}

	// ---- Trend -------------------------------------------------------------------------------------------------------------
	void Trend::Add(double t, double v)
	{
		if (points_.size() >= capacity_) points_.erase(points_.begin());
		points_.push_back({t, v});
	}

	double Trend::Slope() const
	{
		const std::size_t n = points_.size();
		if (n < 2) return 0.0;
		double mt = 0.0, mv = 0.0;
		for (const Point& p : points_) { mt += p.t; mv += p.v; }
		mt /= static_cast<double>(n);
		mv /= static_cast<double>(n);
		double num = 0.0, den = 0.0;
		for (const Point& p : points_) {
			num += (p.t - mt) * (p.v - mv);
			den += (p.t - mt) * (p.t - mt);
		}
		return den > 0.0 ? num / den : 0.0;
	}

	double Trend::R2() const
	{
		const std::size_t n = points_.size();
		if (n < 3) return 0.0;
		double mt = 0.0, mv = 0.0;
		for (const Point& p : points_) { mt += p.t; mv += p.v; }
		mt /= static_cast<double>(n);
		mv /= static_cast<double>(n);
		double stt = 0.0, svv = 0.0, stv = 0.0;
		for (const Point& p : points_) {
			stt += (p.t - mt) * (p.t - mt);
			svv += (p.v - mv) * (p.v - mv);
			stv += (p.t - mt) * (p.v - mv);
		}
		if (stt <= 0.0 || svv <= 0.0) return 0.0;
		return (stv * stv) / (stt * svv);
	}

	double Trend::Rise() const { return points_.size() < 2 ? 0.0 : points_.back().v - points_.front().v; }
	double Trend::Span() const { return points_.size() < 2 ? 0.0 : points_.back().t - points_.front().t; }

	// ---- Anomalies ---------------------------------------------------------------------------------------------------------
	AnomalyDetector::AnomalyDetector(std::string subject, AnomalyRules rules)
		: subject_(std::move(subject)), rules_(rules), growth_(rules.growthWindow)
	{}

	bool AnomalyDetector::Due(Finding::Kind kind, double t)
	{
		const auto it = reportedAt_.find(static_cast<int>(kind));
		if (it != reportedAt_.end() && t - it->second < rules_.cooldown) return false;
		reportedAt_[static_cast<int>(kind)] = t;
		return true;
	}

	std::vector<Finding> AnomalyDetector::Add(const StreamSample& s)
	{
		std::vector<Finding> out;
		growth_.Add(s.t, static_cast<double>(s.live));
		if (!last_ || s.t <= last_->t || s.allocations < last_->allocations) {	// First, or the counters started over
			last_ = s;
			return out;
		}
		const double dt = s.t - last_->t;
		rate_ = static_cast<double>(s.allocations - last_->allocations) / dt;
		byteRate_ = static_cast<double>(s.bytes - last_->bytes) / dt;
		const double liveDelta = static_cast<double>(s.live - last_->live);
		last_ = s;
		++samples_;

		const bool warm = samples_ > rules_.warmup;
		// A spike: way over its own average. The average learns from the spike only a little (alpha), so a new normal
		// takes a while to stop being reported; the cooldown keeps that to one line.
		const bool spiking = avgRate_.primed && rate_ > rules_.spikeMinRate && rate_ > rules_.spikeFactor * avgRate_.value;
		spikeRun_ = spiking ? spikeRun_ + 1 : 0;
		if (warm && spikeRun_ >= rules_.spikeSamples && Due(Finding::Kind::RateSpike, s.t)) {
			char buf[160];
			std::snprintf(buf, sizeof buf, "allocation rate %.0f/s, %.0fx its average (%.0f/s), %s/s", rate_,
						  rate_ / std::max(avgRate_.value, 1.0), avgRate_.value, Mb(byteRate_).c_str());
			out.push_back({Finding::Kind::RateSpike, Finding::Severity::Warning, subject_, buf});
		}
		avgRate_.Add(rate_);

		// Steady growth: a straight line up over the whole window, steep enough and adding enough
		if (warm && growth_.Full()) {
			const double slope = growth_.Slope();
			if (slope > rules_.growthMinBytesPerSec && growth_.R2() > rules_.growthMinR2 && growth_.Rise() > rules_.growthMinRise &&
				Due(Finding::Kind::SteadyGrowth, s.t)) {
				char buf[200];
				std::snprintf(buf, sizeof buf, "grew %s in %.0f s in a straight line (%s/min, r2 %.2f): a leak, or a cache without a bound",
							  Mb(growth_.Rise()).c_str(), growth_.Span(), Mb(slope * 60.0).c_str(), growth_.R2());
				out.push_back({Finding::Kind::SteadyGrowth, Finding::Severity::Warning, subject_, buf});
			}
		}

		// Churn: many allocations that leave nothing behind (temporary objects that a scratch arena or a pool would take)
		if (warm && rate_ > rules_.churnMinRate && std::fabs(liveDelta) < byteRate_ * dt * 0.01 && Due(Finding::Kind::Churn, s.t)) {
			char buf[160];
			std::snprintf(buf, sizeof buf, "churn: %.0f allocations/s (%s/s) with live memory flat", rate_, Mb(byteRate_).c_str());
			out.push_back({Finding::Kind::Churn, Finding::Severity::Info, subject_, buf});
		}
		return out;
	}

	// ---- Pressure ----------------------------------------------------------------------------------------------------------
	const char* LevelName(Level level)
	{
		switch (level) {
			case Level::Normal: return "normal";
			case Level::Elevated: return "elevated";
			case Level::High: return "high";
			case Level::Critical: return "critical";
		}
		return "?";
	}

	Level PressurePolicy::Raw(const PressureInputs& in, const PressureRules& r)
	{
		Level level = Level::Normal;
		for (int l = 3; l >= 1 && level == Level::Normal; --l) {
			if ((in.largestFree && in.largestFree < r.largest[l]) || (in.addressFree && in.addressFree < r.free[l]) ||
				(in.commitAvailable && in.commitAvailable < r.commit[l]))
				level = static_cast<Level>(l);
		}
		if (level == Level::Normal && in.menuBudget > 0 && static_cast<double>(in.menuLive) > r.menuOverBudget * static_cast<double>(in.menuBudget))
			level = Level::Elevated;
		return level;
	}

	Level PressurePolicy::Add(const PressureInputs& in)
	{
		const Level raw = Raw(in, rules_);
		if (raw == level_) {
			pending_ = raw;
			pendingRun_ = 0;
			return level_;
		}
		pendingRun_ = raw == pending_ ? pendingRun_ + 1 : 1;
		pending_ = raw;
		const bool rising = raw > level_;
		const int needed = rising ? (raw == Level::Critical ? 1 : rules_.riseSamples) : rules_.calmSamples;
		if (pendingRun_ >= needed) {
			char buf[200];
			std::snprintf(buf, sizeof buf, "largest free block %s, address space free %s, commit left %s, menu heap %s",
						  Mb(static_cast<double>(in.largestFree)).c_str(), Mb(static_cast<double>(in.addressFree)).c_str(),
						  Mb(static_cast<double>(in.commitAvailable)).c_str(), Mb(static_cast<double>(in.menuLive)).c_str());
			reason_ = buf;
			level_ = raw;
			pendingRun_ = 0;
		}
		return level_;
	}

	// ---- Budget ------------------------------------------------------------------------------------------------------------
	BudgetController::BudgetController(std::size_t start, BudgetRules rules) : rules_(rules), budget_(start)
	{
		budget_ = std::clamp(budget_, rules_.min, std::max(rules_.min, rules_.max));
	}

	void BudgetController::SetRules(const BudgetRules& rules)
	{
		rules_ = rules;
		budget_ = std::clamp(budget_, rules_.min, std::max(rules_.min, rules_.max));
	}

	std::size_t BudgetController::Decide(const BudgetInputs& in, std::string* why)
	{
		if (why) why->clear();
		const std::size_t before = budget_;
		const std::uint64_t hits = in.hits - std::min(in.hits, lastHits_);
		const std::uint64_t trims = in.trims - std::min(in.trims, lastTrims_);
		const bool primed = primed_;
		primed_ = true;

		if (in.level != Level::Normal) recoverTo_ = std::max(recoverTo_, budget_);
		switch (in.level) {
			case Level::Critical:
				budget_ = rules_.min;
				if (why && budget_ != before) *why = "memory pressure critical: caches down to their minimum";
				break;
			case Level::High:
				// Halved each decision rather than dropped at once: what the caches give back is spread over a few
				// passes of the eviction manager instead of one long frame
				budget_ = std::max(rules_.min, budget_ / 2);
				if (why && budget_ != before) *why = "memory pressure high: caches halved";
				break;
			case Level::Elevated: {
				const auto target = static_cast<std::size_t>(static_cast<double>(budget_) * rules_.shrink);
				budget_ = std::max(rules_.min, target);
				if (why && budget_ != before) *why = "memory pressure elevated: caches get less";
				break;
			}
			case Level::Normal: {
				// After pressure: back to the budget from before it, a step per decision
				if (recoverTo_ > budget_) {
					budget_ = std::min({recoverTo_, rules_.max, static_cast<std::size_t>(static_cast<double>(budget_) * rules_.grow) + 1});
					if (budget_ >= recoverTo_ || budget_ >= rules_.max) {
						recoverTo_ = 0;
						if (why) *why = "pressure gone: caches have their memory back";	// Once, at the end (not every step)
					}
					break;
				}
				recoverTo_ = 0;
				// Grow only for caches that are full, being trimmed and used: memory that buys hits
				const double kb = static_cast<double>(in.cacheBytes) / 1024.0;
				const bool full = static_cast<double>(in.cacheBytes) >= rules_.fullAt * static_cast<double>(budget_);
				const bool worth = kb > 0.0 && static_cast<double>(hits) / kb >= rules_.minHitsPerKb;
				if (primed && full && trims > 0 && worth && in.t - lastGrow_ >= rules_.cooldown && budget_ < rules_.max) {
					budget_ = std::min(rules_.max, static_cast<std::size_t>(static_cast<double>(budget_) * rules_.grow) + 1);
					lastGrow_ = in.t;
					if (why) *why = "caches full and in use: more memory for them";
				}
				break;
			}
		}
		// Only count hits and trims since a decision that looked at them
		lastHits_ = in.hits;
		lastTrims_ = in.trims;
		return budget_;
	}

	// ---- Prediction --------------------------------------------------------------------------------------------------------
	void Predictor::Observe(const std::string& level, std::uint64_t privateBytes, std::uint64_t largestFree)
	{
		if (level.empty()) return;
		LevelProfile& p = profiles_[level];
		p.peakPrivate = std::max(p.peakPrivate, privateBytes);
		if (largestFree && (!p.minLargestFree || largestFree < p.minLargestFree)) p.minLargestFree = largestFree;
	}

	void Predictor::EndVisit(const std::string& level)
	{
		if (const auto it = profiles_.find(level); it != profiles_.end()) ++it->second.visits;
	}

	std::optional<LevelProfile> Predictor::Profile(const std::string& level) const
	{
		const auto it = profiles_.find(level);
		if (it == profiles_.end()) return std::nullopt;
		return it->second;
	}

	std::optional<double> Predictor::TimeToLimit(const Trend& trend, double value, double limit)
	{
		if (trend.Count() < 5 || value <= limit) return std::nullopt;
		const double slope = trend.Slope();
		if (slope >= 0.0 || trend.R2() < 0.5) return std::nullopt;
		return (value - limit) / -slope;
	}
}
