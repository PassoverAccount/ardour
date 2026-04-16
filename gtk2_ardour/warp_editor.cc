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
#include <cmath>
#include <cstring>

#include <ytkmm/menu.h>
#include <ytkmm/menuitem.h>
#include <ytkmm/uimanager.h>

#include "gtkmm2ext/utils.h"

#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/session.h"
#include "ardour/transient_analysis.h"
#include "ardour/triggerbox.h"

#include "warp_editor.h"

#include "pbd/i18n.h"

using namespace Gtk;
using namespace ARDOUR;

/* ========================================================================
 * Constants
 * ====================================================================== */

static const int    CANVAS_MIN_HEIGHT     = 80;
static const int    CANVAS_DEFAULT_HEIGHT = 120;
static const double MARKER_HALF_W        = 5.0;  /* half-width of marker triangle */
static const double MARKER_HEIGHT        = 12.0;
static const double WAVEFORM_ALPHA       = 0.65;

/* ========================================================================
 * Construction
 * ====================================================================== */

WarpEditor::WarpEditor ()
	: _trigger (nullptr)
	, _peak_width (0)
	, _dragging (false)
	, _drag_marker_idx (-1)
	, _drag_start_x (0.0)
	, _drag_last_beat (0.0)
	, _view_start_beat (0.0)
	, _view_end_beat (4.0)
{
	_canvas.set_size_request (-1, CANVAS_DEFAULT_HEIGHT);
	_canvas.set_can_focus ();
	_canvas.add_events (Gdk::BUTTON_PRESS_MASK |
	                    Gdk::BUTTON_RELEASE_MASK |
	                    Gdk::POINTER_MOTION_MASK |
	                    Gdk::SCROLL_MASK |
	                    Gdk::KEY_PRESS_MASK);

	_canvas.signal_expose_event   ().connect (sigc::mem_fun (*this, &WarpEditor::on_expose_event));
	_canvas.signal_button_press_event  ().connect (sigc::mem_fun (*this, &WarpEditor::on_button_press), false);
	_canvas.signal_button_release_event().connect (sigc::mem_fun (*this, &WarpEditor::on_button_release), false);
	_canvas.signal_motion_notify_event ().connect (sigc::mem_fun (*this, &WarpEditor::on_motion_notify), false);
	_canvas.signal_scroll_event        ().connect (sigc::mem_fun (*this, &WarpEditor::on_scroll_event), false);

	pack_start (_canvas, true, true, 0);
	show_all ();
}

WarpEditor::~WarpEditor ()
{
}

/* ========================================================================
 * Session / Trigger binding
 * ====================================================================== */

void
WarpEditor::set_session (ARDOUR::Session* s)
{
	SessionHandlePtr::set_session (s);
}

void
WarpEditor::set_trigger (ARDOUR::AudioTrigger* trigger)
{
	_connections.drop_connections ();
	_trigger = trigger;

	if (_trigger) {
		_working_map = _trigger->warp_map ();
		_view_start_beat = 0.0;
		_view_end_beat   = std::max (4.0, _trigger->segment_beatcnt ());

		rebuild_peaks ();
		rebuild_transient_beats ();

		/* Refresh when trigger properties change */
		_trigger->PropertyChanged.connect (_connections,
		    invalidator (*this),
		    [this] (PBD::PropertyChange const&) { invalidate (); },
		    gui_context ());
	} else {
		_peaks_min.clear ();
		_peaks_max.clear ();
		_transient_beats.clear ();
	}

	invalidate ();
}

/* ========================================================================
 * Coordinate helpers
 * ====================================================================== */

double
WarpEditor::pixels_per_beat () const
{
	const double w = (double) _canvas.get_width ();
	const double span = _view_end_beat - _view_start_beat;
	if (span <= 0.0) { return 1.0; }
	return w / span;
}

double
WarpEditor::beat_to_x (double beat) const
{
	return (beat - _view_start_beat) * pixels_per_beat ();
}

double
WarpEditor::x_to_beat (double x) const
{
	return _view_start_beat + x / pixels_per_beat ();
}

int
WarpEditor::marker_at_x (double x, double tolerance) const
{
	int   best_idx = -1;
	double best_d  = tolerance + 1.0;

	auto const& markers = _working_map.markers ();
	for (int i = 0; i < (int) markers.size (); ++i) {
		double mx = beat_to_x (markers[i].beat_pos);
		double d  = std::fabs (x - mx);
		if (d < best_d) {
			best_d   = d;
			best_idx = i;
		}
	}

	return best_idx;
}

/* ========================================================================
 * Peak data and transient data
 * ====================================================================== */

void
WarpEditor::rebuild_peaks ()
{
	if (!_trigger) {
		return;
	}

	int w = _canvas.get_width ();
	if (w <= 0) { w = 400; }
	_peak_width = w;

	_peaks_min.assign (w, 0.f);
	_peaks_max.assign (w, 0.f);

	const samplecnt_t total = (samplecnt_t) _trigger->data_length ();
	if (total <= 0) { return; }

	/* Sample the raw audio data at (w) evenly-spaced positions */
	for (int px = 0; px < w; ++px) {
		const double   frac   = (double) px / (double) (w - 1);
		const samplepos_t idx = (samplepos_t) (frac * (double) (total - 1));
		const Sample* ch = _trigger->audio_data (0);
		if (ch) {
			_peaks_min[px] = ch[idx];
			_peaks_max[px] = ch[idx];
		}
	}

	/* Compute min/max over small windows for a more representative view */
	const int window = std::max (1, (int) (total / w));
	for (int px = 0; px < w; ++px) {
		const samplepos_t start = (samplepos_t) ((double) px / (double) w * (double) total);
		const samplepos_t end   = std::min ((samplepos_t) (start + window), total);
		const Sample* ch = _trigger->audio_data (0);
		if (!ch) { continue; }
		float mn = ch[start], mx = ch[start];
		for (samplepos_t s = start + 1; s < end; ++s) {
			mn = std::min (mn, ch[s]);
			mx = std::max (mx, ch[s]);
		}
		_peaks_min[px] = mn;
		_peaks_max[px] = mx;
	}
}

void
WarpEditor::rebuild_transient_beats ()
{
	_transient_beats.clear ();

	if (!_trigger || !_trigger->data_length ()) {
		return;
	}

	const double beat_count = _trigger->segment_beatcnt ();
	if (beat_count <= 0.0) { return; }

	/* Use the WarpMap to convert from sample positions to beats */
	const WarpMap& wm = _working_map;

	/* Retrieve transient sample positions from the analysis cache */
	/* (stub — in a full implementation this calls TransientAnalysisCache) */

	(void) wm;
}

/* ========================================================================
 * Expose / draw
 * ====================================================================== */

bool
WarpEditor::on_expose_event (GdkEventExpose* /*ev*/)
{
	Cairo::RefPtr<Cairo::Context> cr = _canvas.get_window ()->create_cairo_context ();
	const double w = (double) _canvas.get_width ();
	const double h = (double) _canvas.get_height ();

	/* Background */
	cr->set_source_rgb (0.12, 0.12, 0.12);
	cr->rectangle (0, 0, w, h);
	cr->fill ();

	draw_waveform   (cr, w, h);
	draw_beat_grid  (cr, w, h);
	draw_transients (cr, w, h);
	draw_markers    (cr, w, h);

	return true;
}

void
WarpEditor::draw_waveform (Cairo::RefPtr<Cairo::Context>& cr, double w, double h)
{
	if (_peaks_min.empty () || (int) _peaks_min.size () != (int) w) {
		rebuild_peaks ();
	}

	if (_peaks_min.empty ()) { return; }

	const double mid_y = h * 0.5;
	const double scale = mid_y * 0.9;

	cr->set_source_rgba (0.55, 0.80, 0.55, WAVEFORM_ALPHA);
	cr->set_line_width (1.0);

	for (int px = 0; px < (int) w && px < (int) _peaks_min.size (); ++px) {
		const double y_min = mid_y - (double) _peaks_min[px] * scale;
		const double y_max = mid_y - (double) _peaks_max[px] * scale;
		cr->move_to ((double) px + 0.5, y_max);
		cr->line_to ((double) px + 0.5, y_min);
		cr->stroke ();
	}
}

void
WarpEditor::draw_beat_grid (Cairo::RefPtr<Cairo::Context>& cr, double w, double h)
{
	const double ppb = pixels_per_beat ();
	if (ppb < 2.0) { return; } /* too zoomed out to show grid */

	cr->set_source_rgba (0.35, 0.35, 0.35, 0.6);
	cr->set_line_width (0.5);

	double beat = std::ceil (_view_start_beat);
	while (beat <= _view_end_beat) {
		double x = beat_to_x (beat);
		if (x >= 0 && x <= w) {
			cr->move_to (x, 0);
			cr->line_to (x, h);
			cr->stroke ();
		}
		beat += 1.0;
	}
}

void
WarpEditor::draw_transients (Cairo::RefPtr<Cairo::Context>& cr, double w, double h)
{
	if (_transient_beats.empty ()) { return; }

	cr->set_source_rgba (0.50, 0.50, 0.55, 0.70);
	cr->set_line_width (1.0);

	for (double tb : _transient_beats) {
		double x = beat_to_x (tb);
		if (x < 0 || x > w) { continue; }
		cr->move_to (x, 0);
		cr->line_to (x, h * 0.7);
		cr->stroke ();
	}
}

void
WarpEditor::draw_markers (Cairo::RefPtr<Cairo::Context>& cr, double w, double h)
{
	auto const& markers = _working_map.markers ();
	const bool dragging = _dragging && _drag_marker_idx >= 0;

	for (int i = 0; i < (int) markers.size (); ++i) {
		const double x = beat_to_x (markers[i].beat_pos);
		if (x < -MARKER_HALF_W || x > w + MARKER_HALF_W) { continue; }

		const bool active = dragging && i == _drag_marker_idx;

		/* Vertical line through the entire height */
		cr->set_source_rgba (active ? 1.0 : 0.95,
		                     active ? 0.8 : 0.65,
		                     active ? 0.0 : 0.0,
		                     0.85);
		cr->set_line_width (active ? 2.0 : 1.0);
		cr->move_to (x, 0);
		cr->line_to (x, h);
		cr->stroke ();

		/* Downward-pointing filled triangle at top */
		cr->begin_new_path ();
		cr->move_to (x - MARKER_HALF_W, 0);
		cr->line_to (x + MARKER_HALF_W, 0);
		cr->line_to (x,                 MARKER_HEIGHT);
		cr->close_path ();
		cr->set_source_rgba (active ? 1.0 : 0.95,
		                     active ? 0.8 : 0.65,
		                     0.0, 1.0);
		cr->fill ();
	}
}

/* ========================================================================
 * Interaction
 * ====================================================================== */

bool
WarpEditor::on_button_press (GdkEventButton* ev)
{
	_canvas.grab_focus ();

	if (ev->button == 1) {
		int idx = marker_at_x (ev->x);
		if (idx >= 0) {
			/* Start drag */
			_dragging         = true;
			_drag_marker_idx  = idx;
			_drag_start_x     = ev->x;
			_drag_last_beat   = _working_map.markers ()[idx].beat_pos;
		} else if (ev->type == GDK_2BUTTON_PRESS) {
			/* Double-click → add marker */
			add_marker_at_beat (x_to_beat (ev->x));
		}
	} else if (ev->button == 3) {
		int idx = marker_at_x (ev->x);
		if (idx >= 0) {
			/* Right-click context menu */
			Gtk::Menu* menu = manage (new Gtk::Menu ());
			menu->items ().push_back (
			    MenuElem (_("Delete Marker"),
			              sigc::bind (sigc::mem_fun (*this, &WarpEditor::delete_marker), idx)));
			menu->items ().push_back (
			    MenuElem (_("Snap to Nearest Transient"),
			              sigc::bind (sigc::mem_fun (*this, &WarpEditor::snap_marker_to_transient), idx)));
			menu->popup (ev->button, ev->time);
		}
	}

	return true;
}

bool
WarpEditor::on_button_release (GdkEventButton* ev)
{
	if (_dragging && ev->button == 1) {
		_dragging        = false;
		_drag_marker_idx = -1;
		commit_marker_changes ();
	}
	return true;
}

bool
WarpEditor::on_motion_notify (GdkEventMotion* ev)
{
	if (!_dragging || _drag_marker_idx < 0) { return false; }

	const double new_beat = x_to_beat (ev->x);
	const double clamped  = std::max (0.0, std::min (new_beat, _view_end_beat));

	auto markers = _working_map.markers ();
	if (_drag_marker_idx < (int) markers.size ()) {
		const samplepos_t spos = markers[_drag_marker_idx].sample_pos;
		_working_map.remove_marker (_drag_last_beat);
		_working_map.add_marker (spos, clamped);
		/* Re-look up the index after move (marker may have shifted in sorted list) */
		for (int i = 0; i < (int) _working_map.markers ().size (); ++i) {
			if (_working_map.markers ()[i].beat_pos == clamped) {
				_drag_marker_idx = i;
				break;
			}
		}
		_drag_last_beat = clamped;
		invalidate ();
	}

	return true;
}

bool
WarpEditor::on_scroll_event (GdkEventScroll* ev)
{
	const double span = _view_end_beat - _view_start_beat;

	if (ev->direction == GDK_SCROLL_UP) {
		/* Zoom in around cursor */
		const double cx = x_to_beat (ev->x);
		const double new_span = span * 0.8;
		_view_start_beat = cx - (ev->x / _canvas.get_width ()) * new_span;
		_view_end_beat   = _view_start_beat + new_span;
	} else if (ev->direction == GDK_SCROLL_DOWN) {
		/* Zoom out */
		const double cx = x_to_beat (ev->x);
		const double new_span = span * 1.25;
		_view_start_beat = cx - (ev->x / _canvas.get_width ()) * new_span;
		_view_end_beat   = _view_start_beat + new_span;
	} else if (ev->direction == GDK_SCROLL_LEFT) {
		_view_start_beat -= span * 0.1;
		_view_end_beat   -= span * 0.1;
	} else if (ev->direction == GDK_SCROLL_RIGHT) {
		_view_start_beat += span * 0.1;
		_view_end_beat   += span * 0.1;
	}

	_view_start_beat = std::max (0.0, _view_start_beat);
	invalidate ();
	return true;
}

/* ========================================================================
 * Marker operations
 * ====================================================================== */

void
WarpEditor::add_marker_at_beat (double beat)
{
	if (!_trigger) { return; }

	/* Compute the corresponding source sample via linear interpolation */
	const samplepos_t spos = _working_map.source_sample_at (beat);
	_working_map.add_marker (spos, beat);
	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::delete_marker (int idx)
{
	auto const& markers = _working_map.markers ();
	if (idx < 0 || idx >= (int) markers.size ()) { return; }

	_working_map.remove_marker (markers[idx].beat_pos);
	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::snap_marker_to_transient (int idx)
{
	if (!_trigger || _transient_beats.empty ()) { return; }

	auto const& markers = _working_map.markers ();
	if (idx < 0 || idx >= (int) markers.size ()) { return; }

	const double current_beat = markers[idx].beat_pos;

	/* Find the nearest transient beat */
	double best_beat = _transient_beats[0];
	double best_d    = std::fabs (current_beat - best_beat);

	for (double tb : _transient_beats) {
		double d = std::fabs (current_beat - tb);
		if (d < best_d) {
			best_d    = d;
			best_beat = tb;
		}
	}

	const samplepos_t spos = markers[idx].sample_pos;
	_working_map.remove_marker (current_beat);
	_working_map.add_marker (spos, best_beat);

	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::commit_marker_changes ()
{
	if (_trigger) {
		_trigger->set_warp_map (_working_map);
	}
}

void
WarpEditor::invalidate ()
{
	_canvas.queue_draw ();
}
