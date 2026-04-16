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

#pragma once

#include <algorithm>
#include <vector>

#include "ardour/types.h"
#include "ardour/libardour_visibility.h"

class XMLNode;

namespace ARDOUR {

/** A single warp marker binding an audio sample position to a musical beat
 *  position.  The pair (sample_pos, beat_pos) defines a point in the
 *  clip's internal timeline where "sample_pos audio samples from the start
 *  of the raw audio data" coincides with "beat_pos quarter-note beats from
 *  the clip start in musical time."
 */
struct LIBARDOUR_API WarpMarker {
	samplepos_t sample_pos; /**< Position in source audio samples (from clip start) */
	double      beat_pos;   /**< Corresponding musical position in quarter-note beats */

	WarpMarker () : sample_pos (0), beat_pos (0.0) {}
	WarpMarker (samplepos_t s, double b) : sample_pos (s), beat_pos (b) {}

	bool operator< (WarpMarker const& o) const { return beat_pos < o.beat_pos; }
	bool operator== (WarpMarker const& o) const {
		return sample_pos == o.sample_pos && beat_pos == o.beat_pos;
	}

	XMLNode& get_state () const;
	int      set_state (XMLNode const&, int version);
};

/** An ordered collection of WarpMarkers that defines a non-uniform mapping
 *  between a clip's audio sample timeline and musical beat time.
 *
 *  Between consecutive markers (sᵢ,mᵢ) and (sᵢ₊₁,mᵢ₊₁) the stretch ratio
 *  applied to the audio segment is:
 *
 *    ratio = ((mᵢ₊₁ − mᵢ) × session_samples_per_beat) / (sᵢ₊₁ − sᵢ)
 *
 *  When the map is empty or contains only one marker the default ratio 1.0
 *  is returned (no stretching beyond the existing uniform-stretch path).
 *
 *  Thread safety: WarpMap objects are immutable once published to the
 *  real-time audio thread via an atomic pointer swap (see ElasticStretcher).
 *  The UI always creates a new WarpMap and swaps it in rather than modifying
 *  an existing one in-place.
 */
class LIBARDOUR_API WarpMap {
  public:
	WarpMap ()  = default;
	~WarpMap () = default;

	/* ---- Marker manipulation (call only from non-RT thread) ---- */

	/** Add a marker.  Maintains sorted order by beat_pos.
	 *  If a marker already exists at beat_pos its sample_pos is updated. */
	void add_marker    (samplepos_t sample_pos, double beat_pos);

	/** Remove the marker nearest to beat_pos (within epsilon). */
	void remove_marker (double beat_pos);

	/** Move an existing marker's sample position while keeping beat_pos. */
	void move_marker   (double beat_pos, samplepos_t new_sample_pos);

	/** Remove all markers. */
	void clear () { _markers.clear (); }

	/** Return a copy of all markers (sorted by beat_pos). */
	std::vector<WarpMarker> const& markers () const { return _markers; }

	std::size_t size () const { return _markers.size (); }
	bool        empty () const { return _markers.empty (); }

	/* ---- DSP query methods (safe to call from RT thread) -------- */

	/** Return the local time-stretch ratio for a given playback beat position.
	 *
	 *  @param beat_pos          Current playback position in beats.
	 *  @param session_spb       Session samples-per-beat = sr * 60.0 / bpm.
	 *  @return                  RubberBand time ratio (output / input samples).
	 *                           Returns 1.0 when fewer than two markers exist.
	 */
	double stretch_ratio_at (double beat_pos, double session_spb) const;

	/** Convert a musical beat position to the corresponding source audio
	 *  sample position via linear interpolation between neighbouring markers.
	 *  Returns 0 when fewer than two markers exist. */
	samplepos_t source_sample_at (double beat_pos) const;

	/** Return the index of the segment that contains beat_pos, i.e. the
	 *  index i such that markers[i].beat_pos ≤ beat_pos < markers[i+1].beat_pos.
	 *  Returns -1 if beat_pos is before the first marker or the map has fewer
	 *  than two entries. */
	int segment_for_beat (double beat_pos) const;

	/* ---- Serialisation ---- */
	XMLNode& get_state () const;
	int      set_state (XMLNode const&, int version);

  private:
	std::vector<WarpMarker> _markers; /* always sorted by beat_pos */

	/** Find the insertion position for a given beat position. */
	std::vector<WarpMarker>::const_iterator lower_bound_for (double beat_pos) const;
	std::vector<WarpMarker>::iterator       lower_bound_for (double beat_pos);
};

} /* namespace ARDOUR */
