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

#include <memory>
#include <vector>

#include <ytkmm/box.h>
#include <ytkmm/drawingarea.h>
#include <ytkmm/label.h>
#include <ytkmm/table.h>

#include "pbd/signals.h"

#include "ardour/ardour.h"
#include "ardour/session_handle.h"
#include "ardour/warp_marker.h"
#include "ardour/warp_mode.h"

namespace ARDOUR {
	class AudioTrigger;
	class Session;
}

/** WarpEditor — a waveform overview panel with draggable warp markers.
 *
 *  Displays:
 *   - Waveform peak overview (resampled from peak data)
 *   - Gray vertical tick-marks at auto-detected transient positions
 *   - Orange/gold draggable triangles at user warp markers
 *   - A beat-grid overlay based on the current warp mapping
 *
 *  Interactions:
 *   - Double-click on waveform → add warp marker at that beat position
 *   - Left-drag a marker horizontally → move its beat position
 *   - Right-click marker → context menu (delete / snap to nearest transient)
 *   - Ctrl+Z / Ctrl+Shift+Z → undo/redo warp marker edits
 *
 *  WarpEditor is a Gtk::DrawingArea subclass embedded in the clip properties
 *  panel or in the CueEditor area below the clip waveform view.
 */
class WarpEditor : public Gtk::VBox, public ARDOUR::SessionHandlePtr
{
  public:
	WarpEditor ();
	~WarpEditor ();

	void set_session (ARDOUR::Session*);

	/** Show the warp markers for @p trigger.  Pass nullptr to clear. */
	void set_trigger (ARDOUR::AudioTrigger* trigger);

	/** Force a full redraw (e.g. after marker or tempo-map change). */
	void invalidate ();

  private:
	/* ---- GTK signal handlers ---- */
	bool on_expose_event   (GdkEventExpose*   ev);
	bool on_button_press   (GdkEventButton*   ev);
	bool on_button_release (GdkEventButton*   ev);
	bool on_motion_notify  (GdkEventMotion*   ev);
	bool on_scroll_event   (GdkEventScroll*   ev);

	void draw_waveform   (Cairo::RefPtr<Cairo::Context>&, double w, double h);
	void draw_transients (Cairo::RefPtr<Cairo::Context>&, double w, double h);
	void draw_beat_grid  (Cairo::RefPtr<Cairo::Context>&, double w, double h);
	void draw_markers    (Cairo::RefPtr<Cairo::Context>&, double w, double h);

	/* ---- coordinate helpers ---- */

	/** Convert a beat position to a pixel X coordinate. */
	double beat_to_x (double beat) const;

	/** Convert pixel X to a beat position. */
	double x_to_beat (double x) const;

	/** Find the marker (index) under pixel position @p x, within @p tolerance
	 *  pixels.  Returns -1 if none. */
	int marker_at_x (double x, double tolerance = 6.0) const;

	/* ---- Marker operations ---- */
	void add_marker_at_beat      (double beat);
	void delete_marker            (int idx);
	void snap_marker_to_transient (int idx);
	void commit_marker_changes    ();

	/* ---- Members ---- */
	Gtk::DrawingArea       _canvas;

	ARDOUR::AudioTrigger*  _trigger;
	ARDOUR::WarpMap        _working_map; /* local copy being edited */

	/* Peak data for waveform overview (interleaved min/max pairs per pixel) */
	std::vector<float>     _peaks_min;
	std::vector<float>     _peaks_max;
	int                    _peak_width; /* width when peaks were computed */

	/* Transient positions from analysis (in beats, relative to clip start) */
	std::vector<double>    _transient_beats;

	/* Drag state */
	bool                   _dragging;
	int                    _drag_marker_idx;
	double                 _drag_start_x;
	double                 _drag_last_beat;

	/* View range */
	double                 _view_start_beat;
	double                 _view_end_beat;

	/* Horizontal zoom (pixels per beat) — derived from view range */
	double pixels_per_beat () const;

	void rebuild_peaks ();
	void rebuild_transient_beats ();

	PBD::ScopedConnectionList _connections;
};
