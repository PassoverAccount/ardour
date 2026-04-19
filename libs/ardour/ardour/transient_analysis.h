/*
 * Copyright (C) 2026 Derson Productions <support@dersonproductions.us>
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

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"
#include "ardour/warp_marker.h"

namespace ARDOUR {

class AudioSource;
class AudioReadable;

/** Per-source cache for transient/onset positions detected by TransientDetector.
 *
 *  The cache stores the AnalysisFeatureList (list<samplepos_t>) for every
 *  analysed audio source channel, keyed by the source's PBD::ID and the
 *  channel index.
 *
 *  Results are computed once (offline) and stored:
 *   1. In-memory in this object (for the current session lifetime).
 *   2. In the session XML as an optional child element of the AudioSource node,
 *      so that subsequent session loads avoid re-running the detector.
 *
 *  Callers schedule analysis via schedule_analysis(); the actual work runs on
 *  the TriggerBoxThread or Session::non_realtime_work pool and delivers results
 *  via a callback on completion (AnalysisComplete signal).
 *
 *  Thread safety: schedule_analysis() may be called from any thread.
 *  The internal results map is protected by a read-write lock.  RT callers
 *  should use get_transients() which acquires a read lock briefly.
 */
class LIBARDOUR_API TransientAnalysisCache {
  public:
	TransientAnalysisCache ();
	~TransientAnalysisCache ();

	/** Schedule background transient detection for all channels of @p source.
	 *  If results are already cached for this source this is a no-op.
	 *  On completion, AnalysisComplete is emitted with the source ID. */
	void schedule_analysis (std::shared_ptr<AudioSource> source,
	                        float                        sample_rate);

	/** Retrieve cached transient positions for @p source_id / @p channel.
	 *  Returns an empty list if the analysis has not completed yet. */
	AnalysisFeatureList get_transients (PBD::ID const& source_id,
	                                    uint32_t       channel) const;

	/** Return true iff we have cached results for every channel of @p source. */
	bool has_results (PBD::ID const& source_id) const;

	/** Remove cached results for @p source_id. */
	void drop_results (PBD::ID const& source_id);

	/** Build an initial WarpMap for a clip given detected transients and an
	 *  estimated BPM/beat-count.  Places markers at detected downbeat positions
	 *  (every @p beats_per_bar beats), snapping each to the nearest transient
	 *  within @p snap_threshold_ms milliseconds when one is available.
	 *
	 *  The first marker is always at (sample=0, beat=0) and the last at
	 *  (sample=total_length, beat=beat_count).
	 *
	 *  @param transients       Detected transient sample positions.
	 *  @param total_samples    Total number of samples in the clip.
	 *  @param beat_count       Total beat count of the clip (from BPM estimate).
	 *  @param beats_per_bar    Time signature numerator.
	 *  @param sample_rate      Session sample rate.
	 *  @param snap_threshold_ms  Maximum snap distance (default 50 ms).
	 *  @return                 A WarpMap with anchor markers placed.
	 */
	static WarpMap build_initial_warp_map (AnalysisFeatureList const& transients,
	                                        samplecnt_t                total_samples,
	                                        double                     beat_count,
	                                        int                        beats_per_bar,
	                                        float                      sample_rate,
	                                        float                      snap_threshold_ms = 50.f);

	/** Emitted (on a non-RT thread) when analysis for a source completes.
	 *  Argument is the source PBD::ID. */
	PBD::Signal<void(PBD::ID)> AnalysisComplete;

  private:
	struct SourceResults {
		std::vector<AnalysisFeatureList> channels; /* one list per channel */
	};

	mutable PBD::RWLock                        _lock;
	std::map<PBD::ID, SourceResults>           _cache;
	std::shared_ptr<std::atomic<bool>>         _stop;

	void run_analysis (std::shared_ptr<AudioSource> source,
	                   float                         sample_rate,
	                   std::shared_ptr<std::atomic<bool>> stop_flag);
};

} /* namespace ARDOUR */
