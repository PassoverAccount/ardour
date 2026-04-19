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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "pbd/xml++.h"

#include "ardour/warp_marker.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;

/* ========================================================================
 * WarpMarker serialisation
 * ====================================================================== */

XMLNode&
WarpMarker::get_state () const
{
	XMLNode* node = new XMLNode (X_("WarpMarker"));
	node->set_property (X_("sample"), sample_pos);
	node->set_property (X_("beat"),   beat_pos);
	return *node;
}

int
WarpMarker::set_state (XMLNode const& node, int /*version*/)
{
	if (!node.get_property (X_("sample"), sample_pos)) {
		return -1;
	}
	if (!node.get_property (X_("beat"), beat_pos)) {
		return -1;
	}
	return 0;
}

/* ========================================================================
 * WarpMap helpers
 * ====================================================================== */

static const double BEAT_EPSILON = 1e-9;

vector<WarpMarker>::const_iterator
WarpMap::lower_bound_for (double beat_pos) const
{
	WarpMarker key;
	key.beat_pos = beat_pos;
	return lower_bound (_markers.begin (), _markers.end (), key);
}

vector<WarpMarker>::iterator
WarpMap::lower_bound_for (double beat_pos)
{
	WarpMarker key;
	key.beat_pos = beat_pos;
	return lower_bound (_markers.begin (), _markers.end (), key);
}

/* ========================================================================
 * WarpMap — marker manipulation
 * ====================================================================== */

void
WarpMap::add_marker (samplepos_t sample_pos, double beat_pos)
{
	auto it = lower_bound_for (beat_pos);

	if (it != _markers.end () &&
	    fabs (it->beat_pos - beat_pos) < BEAT_EPSILON) {
		/* Update existing marker at same beat position */
		it->sample_pos = sample_pos;
		return;
	}

	_markers.insert (it, WarpMarker (sample_pos, beat_pos));
}

void
WarpMap::remove_marker (double beat_pos)
{
	auto it = lower_bound_for (beat_pos);

	if (it == _markers.end ()) {
		return;
	}

	/* Find the nearest marker (not just the lower-bound) */
	if (it != _markers.begin ()) {
		auto prev = it;
		--prev;
		if (fabs (prev->beat_pos - beat_pos) < fabs (it->beat_pos - beat_pos)) {
			it = prev;
		}
	}

	_markers.erase (it);
}

void
WarpMap::move_marker (double beat_pos, samplepos_t new_sample_pos)
{
	auto it = lower_bound_for (beat_pos);

	if (it == _markers.end ()) {
		return;
	}

	if (fabs (it->beat_pos - beat_pos) > BEAT_EPSILON) {
		/* No exact match; try the preceding element */
		if (it != _markers.begin ()) {
			--it;
			if (fabs (it->beat_pos - beat_pos) > BEAT_EPSILON) {
				return; /* nothing found */
			}
		} else {
			return;
		}
	}

	it->sample_pos = new_sample_pos;
}

/* ========================================================================
 * WarpMap — DSP queries (safe from RT thread)
 * ====================================================================== */

int
WarpMap::segment_for_beat (double beat_pos) const
{
	if (_markers.size () < 2) {
		return -1;
	}

	/* Find the first marker with beat_pos > our query beat */
	auto it = lower_bound_for (beat_pos + BEAT_EPSILON);

	if (it == _markers.begin () || it == _markers.end ()) {
		/* beat_pos is before the first marker or after all markers */
		return -1;
	}

	return static_cast<int> (distance (_markers.begin (), it) - 1);
}

double
WarpMap::stretch_ratio_at (double beat_pos, double session_spb) const
{
	if (_markers.size () < 2) {
		return 1.0;
	}

	int seg = segment_for_beat (beat_pos);

	if (seg < 0) {
		/* Outside all markers; use the ratio of the closest edge segment */
		if (beat_pos < _markers.front ().beat_pos) {
			seg = 0;
		} else {
			seg = static_cast<int> (_markers.size ()) - 2;
		}
	}

	WarpMarker const& m0 = _markers[seg];
	WarpMarker const& m1 = _markers[seg + 1];

	const double beat_delta   = m1.beat_pos   - m0.beat_pos;
	const double sample_delta = static_cast<double> (m1.sample_pos - m0.sample_pos);

	if (sample_delta < 1.0 || beat_delta < BEAT_EPSILON) {
		return 1.0;
	}

	/* ratio = target_samples / source_samples
	 *       = (beat_delta * session_spb) / sample_delta
	 */
	return (beat_delta * session_spb) / sample_delta;
}

samplepos_t
WarpMap::source_sample_at (double beat_pos) const
{
	if (_markers.size () < 2) {
		return 0;
	}

	int seg = segment_for_beat (beat_pos);

	if (seg < 0) {
		if (beat_pos <= _markers.front ().beat_pos) {
			return _markers.front ().sample_pos;
		} else {
			return _markers.back ().sample_pos;
		}
	}

	WarpMarker const& m0 = _markers[seg];
	WarpMarker const& m1 = _markers[seg + 1];

	const double beat_span = m1.beat_pos - m0.beat_pos;
	if (beat_span < BEAT_EPSILON) {
		return m0.sample_pos;
	}

	const double t = (beat_pos - m0.beat_pos) / beat_span;
	return m0.sample_pos +
	       static_cast<samplepos_t> (t * static_cast<double> (m1.sample_pos - m0.sample_pos));
}

/* ========================================================================
 * WarpMap — serialisation
 * ====================================================================== */

XMLNode&
WarpMap::get_state () const
{
	XMLNode* node = new XMLNode (X_("WarpMap"));

	for (WarpMarker const& m : _markers) {
		node->add_child_nocopy (m.get_state ());
	}

	return *node;
}

int
WarpMap::set_state (XMLNode const& node, int version)
{
	_markers.clear ();

	XMLNodeList const& children = node.children ();

	for (XMLNode const* child : children) {
		if (child->name () == X_("WarpMarker")) {
			WarpMarker m;
			if (m.set_state (*child, version) == 0) {
				_markers.push_back (m);
			}
		}
	}

	/* Ensure sorted order.  The XML should already be in sorted order
	 * (we always write it that way), but we sort here defensively to
	 * maintain the class invariant that _markers is always sorted by
	 * beat_pos — which is relied upon by the binary-search helpers. */
	sort (_markers.begin (), _markers.end ());

	return 0;
}
