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
#include <cmath>
#include <cstring>

#include <ytkmm/menu.h>
#include <ytkmm/menuitem.h>
#include <ytkmm/uimanager.h>
#include <ytkmm/filechooserdialog.h>
#include <ytkmm/filefilter.h>
#include <ytkmm/stock.h>
#include <ydk/gdkkeysyms-compat.h>

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/utils.h"

#include "pbd/xml++.h"

#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/session.h"
#include "ardour/transient_analysis.h"
#include "ardour/triggerbox.h"

#include "temporal/tempo.h"

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
	, _selected_marker_idx (-1)
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
	_canvas.signal_key_press_event     ().connect (sigc::mem_fun (*this, &WarpEditor::on_key_press), false);

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
	_selected_marker_idx = -1;
	_undo_stack.clear ();
	_redo_stack.clear ();

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

		/* Refresh transients when analysis completes */
		_trigger->TransientAnalysisComplete.connect (_connections,
		    invalidator (*this),
		    [this] () {
			    rebuild_transient_beats ();
			    invalidate ();
		    },
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
	const Sample* ch = _trigger->audio_data (0);
	for (int px = 0; px < w; ++px) {
		const double   frac   = (w > 1) ? ((double) px / (double) (w - 1)) : 0.0;
		const samplepos_t idx = (samplepos_t) (frac * (double) (total - 1));
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

	const samplecnt_t total = (samplecnt_t) _trigger->data_length ();
	const double samples_per_beat = (double) total / beat_count;

	if (samples_per_beat <= 0.0) { return; }

	/* Retrieve cached transient positions from the analysis cache */
	AnalysisFeatureList transients = _trigger->get_transients ();

	for (samplepos_t t : transients) {
		if (t >= 0 && t < total) {
			const double beat = (double) t / samples_per_beat;
			_transient_beats.push_back (beat);
		}
	}
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
	draw_ratio_overlay (cr, w, h);
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
WarpEditor::draw_ratio_overlay (Cairo::RefPtr<Cairo::Context>& cr, double w, double h)
{
	if (!_trigger || _working_map.size () < 2) { return; }

	/* Compute the session samples-per-beat.  This lets us call stretch_ratio_at(). */
	double bpm = 120.0;
	if (_session) {
		Temporal::TempoPoint const& tp = Temporal::TempoMap::use()->tempo_at (timepos_t (0));
		bpm = tp.quarter_notes_per_minute ();
	}
	const double sr = _trigger->box ().session ().sample_rate ();
	const double spb = sr * 60.0 / bpm;

	/* Draw a semi-transparent ratio graph in the lower portion of the editor.
	 *  ratio=1.0 sits at the bottom; larger ratios go up. Cap display at 2.0. */
	const double graph_bottom = h;
	const double graph_height = h * 0.25;
	const double ratio_max = 2.0;

	cr->set_source_rgba (0.8, 0.4, 0.1, 0.4);
	cr->set_line_width (1.5);

	bool first = true;
	for (int px = 0; px < (int) w; ++px) {
		const double beat = x_to_beat ((double) px);
		const double ratio = _working_map.stretch_ratio_at (beat, spb);
		const double clamped = std::min (ratio, ratio_max);
		const double y = graph_bottom - (clamped / ratio_max) * graph_height;

		if (first) {
			cr->move_to ((double) px, y);
			first = false;
		} else {
			cr->line_to ((double) px, y);
		}
	}
	cr->stroke ();

	/* Draw a thin baseline at ratio=1.0 */
	const double base_y = graph_bottom - (1.0 / ratio_max) * graph_height;
	cr->set_source_rgba (0.6, 0.6, 0.6, 0.3);
	cr->set_line_width (0.5);
	cr->move_to (0, base_y);
	cr->line_to (w, base_y);
	cr->stroke ();
}

void
WarpEditor::draw_markers (Cairo::RefPtr<Cairo::Context>& cr, double w, double h)
{
	auto const& markers = _working_map.markers ();
	const bool dragging = _dragging && _drag_marker_idx >= 0;

	for (int i = 0; i < (int) markers.size (); ++i) {
		const double x = beat_to_x (markers[i].beat_pos);
		if (x < -MARKER_HALF_W || x > w + MARKER_HALF_W) { continue; }

		const bool active   = dragging && i == _drag_marker_idx;
		const bool selected = (!dragging) && i == _selected_marker_idx;

		/* Vertical line through the entire height */
		if (active) {
			cr->set_source_rgba (1.0, 0.8, 0.0, 0.85);
			cr->set_line_width (2.0);
		} else if (selected) {
			cr->set_source_rgba (0.2, 0.7, 1.0, 0.90);
			cr->set_line_width (2.0);
		} else {
			cr->set_source_rgba (0.95, 0.65, 0.0, 0.85);
			cr->set_line_width (1.0);
		}
		cr->move_to (x, 0);
		cr->line_to (x, h);
		cr->stroke ();

		/* Downward-pointing filled triangle at top */
		cr->begin_new_path ();
		cr->move_to (x - MARKER_HALF_W, 0);
		cr->line_to (x + MARKER_HALF_W, 0);
		cr->line_to (x,                 MARKER_HEIGHT);
		cr->close_path ();
		if (active) {
			cr->set_source_rgba (1.0, 0.8, 0.0, 1.0);
		} else if (selected) {
			cr->set_source_rgba (0.2, 0.7, 1.0, 1.0);
		} else {
			cr->set_source_rgba (0.95, 0.65, 0.0, 1.0);
		}
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
			/* Select + start drag */
			select_marker (idx);
			push_undo ();
			_dragging         = true;
			_drag_marker_idx  = idx;
			_drag_start_x     = ev->x;
			_drag_last_beat   = _working_map.markers ()[idx].beat_pos;
		} else if (ev->type == GDK_2BUTTON_PRESS) {
			/* Double-click → add marker */
			push_undo ();
			add_marker_at_beat (x_to_beat (ev->x));
		} else {
			/* Click on empty space → deselect */
			select_marker (-1);
		}
	} else if (ev->button == 3) {
		int idx = marker_at_x (ev->x);
		if (idx >= 0) {
			select_marker (idx);
			/* Right-click context menu */
			Gtk::Menu* menu = manage (new Gtk::Menu ());
			menu->items ().push_back (
			    Gtk::Menu_Helpers::MenuElem (_("Delete Marker"),
			              sigc::bind (sigc::mem_fun (*this, &WarpEditor::delete_marker), idx)));
			menu->items ().push_back (
			    Gtk::Menu_Helpers::MenuElem (_("Snap to Nearest Transient"),
			              sigc::bind (sigc::mem_fun (*this, &WarpEditor::snap_marker_to_transient), idx)));
			menu->items ().push_back (Gtk::Menu_Helpers::SeparatorElem ());
			menu->items ().push_back (
			    Gtk::Menu_Helpers::MenuElem (_("Save Warp Preset..."),
			              sigc::mem_fun (*this, &WarpEditor::save_preset)));
			menu->items ().push_back (
			    Gtk::Menu_Helpers::MenuElem (_("Load Warp Preset..."),
			              sigc::mem_fun (*this, &WarpEditor::load_preset)));
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

	/* Select the newly-added marker */
	auto const& markers = _working_map.markers ();
	for (int i = 0; i < (int) markers.size (); ++i) {
		if (markers[i].beat_pos == beat) {
			_selected_marker_idx = i;
			break;
		}
	}

	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::delete_marker (int idx)
{
	auto const& markers = _working_map.markers ();
	if (idx < 0 || idx >= (int) markers.size ()) { return; }

	push_undo ();
	_working_map.remove_marker (markers[idx].beat_pos);
	if (_selected_marker_idx == idx) {
		_selected_marker_idx = -1;
	} else if (_selected_marker_idx > idx) {
		_selected_marker_idx--;
	}
	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::snap_marker_to_transient (int idx)
{
	if (!_trigger || _transient_beats.empty ()) { return; }

	auto const& markers = _working_map.markers ();
	if (idx < 0 || idx >= (int) markers.size ()) { return; }

	push_undo ();
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

/* ========================================================================
 * Undo / Redo
 * ====================================================================== */

void
WarpEditor::push_undo ()
{
	_undo_stack.push_back (_working_map);
	if (_undo_stack.size () > MAX_UNDO_DEPTH) {
		_undo_stack.erase (_undo_stack.begin ());
	}
	_redo_stack.clear ();
}

void
WarpEditor::undo ()
{
	if (_undo_stack.empty ()) { return; }

	_redo_stack.push_back (_working_map);
	_working_map = _undo_stack.back ();
	_undo_stack.pop_back ();
	_selected_marker_idx = -1;
	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::redo ()
{
	if (_redo_stack.empty ()) { return; }

	_undo_stack.push_back (_working_map);
	_working_map = _redo_stack.back ();
	_redo_stack.pop_back ();
	_selected_marker_idx = -1;
	commit_marker_changes ();
	invalidate ();
}

/* ========================================================================
 * Selection / keyboard marker operations
 * ====================================================================== */

void
WarpEditor::select_marker (int idx)
{
	if (_selected_marker_idx != idx) {
		_selected_marker_idx = idx;
		invalidate ();
	}
}

void
WarpEditor::nudge_selected_marker (double beat_delta)
{
	if (_selected_marker_idx < 0) { return; }

	auto const& markers = _working_map.markers ();
	if (_selected_marker_idx >= (int) markers.size ()) { return; }

	push_undo ();
	const double old_beat = markers[_selected_marker_idx].beat_pos;
	const double new_beat = std::max (0.0, old_beat + beat_delta);
	const samplepos_t spos = markers[_selected_marker_idx].sample_pos;

	_working_map.remove_marker (old_beat);
	_working_map.add_marker (spos, new_beat);

	/* Re-locate the selection index after the sorted insert */
	auto const& new_markers = _working_map.markers ();
	for (int i = 0; i < (int) new_markers.size (); ++i) {
		if (std::fabs (new_markers[i].beat_pos - new_beat) < 1e-9) {
			_selected_marker_idx = i;
			break;
		}
	}

	commit_marker_changes ();
	invalidate ();
}

void
WarpEditor::delete_selected_marker ()
{
	if (_selected_marker_idx >= 0) {
		delete_marker (_selected_marker_idx);
	}
}

/* ========================================================================
 * Keyboard handler
 * ====================================================================== */

bool
WarpEditor::on_key_press (GdkEventKey* ev)
{
	const bool ctrl = (ev->state & GDK_CONTROL_MASK);
	const bool shift = (ev->state & GDK_SHIFT_MASK);

	switch (ev->keyval) {
	case GDK_z:
	case GDK_Z:
		if (ctrl && shift) {
			redo ();
		} else if (ctrl) {
			undo ();
		}
		return true;

	case GDK_y:
	case GDK_Y:
		if (ctrl) {
			redo ();
		}
		return true;

	case GDK_Delete:
	case GDK_BackSpace:
		delete_selected_marker ();
		return true;

	case GDK_Insert:
		/* Add marker at center of visible range */
		{
			const double center_beat = (_view_start_beat + _view_end_beat) * 0.5;
			push_undo ();
			add_marker_at_beat (center_beat);
		}
		return true;

	case GDK_Left:
		if (_selected_marker_idx >= 0) {
			nudge_selected_marker (shift ? -0.25 : -0.01);
		} else {
			/* Pan left */
			const double span = _view_end_beat - _view_start_beat;
			_view_start_beat -= span * 0.05;
			_view_end_beat   -= span * 0.05;
			_view_start_beat = std::max (0.0, _view_start_beat);
			invalidate ();
		}
		return true;

	case GDK_Right:
		if (_selected_marker_idx >= 0) {
			nudge_selected_marker (shift ? 0.25 : 0.01);
		} else {
			/* Pan right */
			const double span = _view_end_beat - _view_start_beat;
			_view_start_beat += span * 0.05;
			_view_end_beat   += span * 0.05;
			invalidate ();
		}
		return true;

	case GDK_Escape:
		select_marker (-1);
		return true;

	case GDK_plus:
	case GDK_equal:
		/* Zoom in */
		{
			const double center = (_view_start_beat + _view_end_beat) * 0.5;
			const double span = (_view_end_beat - _view_start_beat) * 0.8;
			_view_start_beat = std::max (0.0, center - span * 0.5);
			_view_end_beat   = center + span * 0.5;
			invalidate ();
		}
		return true;

	case GDK_minus:
		/* Zoom out */
		{
			const double center = (_view_start_beat + _view_end_beat) * 0.5;
			const double span = (_view_end_beat - _view_start_beat) * 1.25;
			_view_start_beat = std::max (0.0, center - span * 0.5);
			_view_end_beat   = center + span * 0.5;
			invalidate ();
		}
		return true;

	default:
		break;
	}

	return false;
}

/* ========================================================================
 * Preset save/load
 * ====================================================================== */

void
WarpEditor::save_preset ()
{
	if (_working_map.empty ()) { return; }

	Gtk::FileChooserDialog dlg (_("Save Warp Preset"), Gtk::FILE_CHOOSER_ACTION_SAVE);
	dlg.add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
	dlg.add_button (Gtk::Stock::SAVE,   Gtk::RESPONSE_ACCEPT);
	dlg.set_current_name ("warp-preset.xml");

	Gtk::FileFilter filter;
	filter.set_name (_("Warp Presets (*.xml)"));
	filter.add_pattern ("*.xml");
	dlg.add_filter (filter);

	if (dlg.run () == Gtk::RESPONSE_ACCEPT) {
		XMLNode& root = _working_map.get_state ();
		XMLTree tree;
		tree.set_root (&root);
		tree.write (dlg.get_filename ());
	}
}

void
WarpEditor::load_preset ()
{
	Gtk::FileChooserDialog dlg (_("Load Warp Preset"), Gtk::FILE_CHOOSER_ACTION_OPEN);
	dlg.add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
	dlg.add_button (Gtk::Stock::OPEN,   Gtk::RESPONSE_ACCEPT);

	Gtk::FileFilter filter;
	filter.set_name (_("Warp Presets (*.xml)"));
	filter.add_pattern ("*.xml");
	dlg.add_filter (filter);

	if (dlg.run () == Gtk::RESPONSE_ACCEPT) {
		XMLTree tree;
		if (tree.read (dlg.get_filename ())) {
			XMLNode const* root = tree.root ();
			if (root && root->name () == "WarpMap") {
				push_undo ();
				_working_map.set_state (*root, 0);
				_selected_marker_idx = -1;
				commit_marker_changes ();
				invalidate ();
			}
		}
	}
}
