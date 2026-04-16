/*
 * Copyright (C) 2024 Paul Davis <paul@linuxaudiosystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include "pbd/rwlock.h"

#include "ardour/audiosource.h"
#include "ardour/transient_analysis.h"
#include "ardour/transient_detector.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;

/* ========================================================================
 * TransientAnalysisCache
 * ====================================================================== */

TransientAnalysisCache::TransientAnalysisCache ()
	: _stop (std::make_shared<std::atomic<bool>> (false))
{
}

TransientAnalysisCache::~TransientAnalysisCache ()
{
	/* Signal all in-flight analysis threads to stop.  The shared_ptr
	 * keeps the atomic alive even after this object is destroyed. */
	_stop->store (true);
}

void
TransientAnalysisCache::schedule_analysis (shared_ptr<AudioSource> source,
                                           float                   sample_rate)
{
	if (!source) {
		return;
	}

	{
		PBD::RWLock::ReaderLock rl (_lock);
		if (_cache.count (source->id ())) {
			return; /* already done */
		}
	}

	/* Run the detection on a pool thread so we never block the RT thread.
	 * We capture stop_flag by value (shared_ptr) so it remains valid even
	 * if the TransientAnalysisCache is destroyed before the thread finishes. */
	auto stop_flag = _stop;
	std::thread t ([this, source, sample_rate, stop_flag] () {
		run_analysis (source, sample_rate, stop_flag);
	});
	t.detach ();
}

void
TransientAnalysisCache::run_analysis (shared_ptr<AudioSource> source,
                                      float                   sample_rate,
                                      shared_ptr<atomic<bool>> stop_flag)
{
	if (stop_flag->load ()) {
		return;
	}

	const uint32_t nchans = source->n_channels ();
	SourceResults  results;
	results.channels.resize (nchans);

	for (uint32_t c = 0; c < nchans; ++c) {
		if (stop_flag->load ()) {
			return;
		}

		TransientDetector detector (sample_rate);
		detector.set_threshold (0.35f);

		AnalysisFeatureList& feats = results.channels[c];

		/* AudioAnalyser::run() expects a file path and an AudioReadable*;
		 * AudioSource implements AudioReadable so we pass it directly. */
		if (detector.run (source->name (), source.get (), c, feats) != 0) {
			feats.clear ();
		} else {
			TransientDetector::cleanup_transients (feats, sample_rate, 3.0f);
		}
	}

	/* Do not access `this` after stop_flag is set — the cache may be gone.
	 * Check once more before touching any member data. */
	if (stop_flag->load ()) {
		return;
	}

	{
		PBD::RWLock::WriterLock wl (_lock);
		_cache[source->id ()] = move (results);
	}

	AnalysisComplete (source->id ()); /* EMIT SIGNAL */
}

AnalysisFeatureList
TransientAnalysisCache::get_transients (PBD::ID const& source_id,
                                        uint32_t       channel) const
{
	PBD::RWLock::ReaderLock rl (_lock);

	auto it = _cache.find (source_id);
	if (it == _cache.end ()) {
		return AnalysisFeatureList ();
	}

	SourceResults const& sr = it->second;
	if (channel >= sr.channels.size ()) {
		return AnalysisFeatureList ();
	}

	return sr.channels[channel];
}

bool
TransientAnalysisCache::has_results (PBD::ID const& source_id) const
{
	PBD::RWLock::ReaderLock rl (_lock);
	return _cache.count (source_id) != 0;
}

void
TransientAnalysisCache::drop_results (PBD::ID const& source_id)
{
	PBD::RWLock::WriterLock wl (_lock);
	_cache.erase (source_id);
}

/* ========================================================================
 * Build initial WarpMap from detected transients
 * ====================================================================== */

WarpMap
TransientAnalysisCache::build_initial_warp_map (
    AnalysisFeatureList const& transients,
    samplecnt_t                total_samples,
    double                     beat_count,
    int                        beats_per_bar,
    float                      sample_rate,
    float                      snap_threshold_ms)
{
	WarpMap map;

	if (total_samples <= 0 || beat_count <= 0.0) {
		return map;
	}

	/* Always anchor start and end */
	map.add_marker (0,             0.0);
	map.add_marker (total_samples, beat_count);

	if (beats_per_bar < 1) {
		beats_per_bar = 4;
	}

	const float snap_samples = snap_threshold_ms * sample_rate / 1000.f;
	const double samples_per_beat = (double) total_samples / beat_count;

	/* Place markers at downbeats (every beats_per_bar beats) */
	double beat = (double) beats_per_bar;
	while (beat < beat_count - (double) beats_per_bar * 0.5) {
		samplepos_t nominal = (samplepos_t) (beat * samples_per_beat);

		/* Snap to nearest transient if one is close enough */
		samplepos_t best    = nominal;
		samplecnt_t best_d  = (samplecnt_t) snap_samples + 1;

		for (samplepos_t t : transients) {
			samplecnt_t d = (samplecnt_t) std::abs ((long long) (t - nominal));
			if (d < best_d) {
				best_d = d;
				best   = t;
			}
		}

		map.add_marker (best, beat);
		beat += (double) beats_per_bar;
	}

	return map;
}
