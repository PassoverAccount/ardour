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
#include <cstring>

#include <rubberband/RubberBandStretcher.h>

#include "pbd/error.h"

#include "ardour/elastic_stretcher.h"

#include "pbd/i18n.h"

using namespace std;
using namespace RubberBand;
using namespace ARDOUR;

static const samplecnt_t ES_BLOCKSIZE = 1024;

/* ========================================================================
 * Construction / destruction
 * ====================================================================== */

ElasticStretcher::ElasticStretcher (double         sample_rate,
                                    uint32_t       nchannels,
                                    WarpMode       mode,
                                    WarpMap const* warp_map)
	: _sample_rate (sample_rate)
	, _nchannels (nchannels)
	, _mode (mode)
	, _stretcher (nullptr)
	, _repitch_read_pos (0.0)
	, _repitch_last_ratio (1.0)
	, _xfade_pending (false)
	, _padding_done (false)
	, _to_drop (0)
{
	if (warp_map) {
		_warp_map_copy = *warp_map;
	}

	_xfade_tail.resize (_nchannels, vector<float> (CROSSFADE_LEN, 0.f));

	/* Pre-allocate RT scratch buffers so process() never calls malloc */
	_in_ptrs.resize (_nchannels, nullptr);
	_discard_buf.resize (_nchannels * ES_BLOCKSIZE, 0.f);
	_discard_ptrs.resize (_nchannels, nullptr);
	for (uint32_t c = 0; c < _nchannels; ++c) {
		_discard_ptrs[c] = _discard_buf.data () + c * ES_BLOCKSIZE;
	}

	if (_mode != WarpMode::RePitch) {
		build_stretcher ();
	}
}

ElasticStretcher::~ElasticStretcher ()
{
	destroy_stretcher ();
}

/* ========================================================================
 * Internal helpers
 * ====================================================================== */

void
ElasticStretcher::build_stretcher ()
{
	destroy_stretcher ();

	RubberBandStretcher::Options opts = rb_options_for_warp_mode (_mode);
	if (opts == RubberBandStretcher::Option (0)) {
		/* RePitch mode or bad mode — no stretcher */
		return;
	}

	_stretcher = new RubberBandStretcher (
	    static_cast<size_t> (_sample_rate),
	    _nchannels,
	    opts,
	    1.0,  /* initial time ratio */
	    1.0   /* pitch scale */
	);

	_stretcher->setMaxProcessSize (ES_BLOCKSIZE);
	_padding_done = false;
	_to_drop      = 0;
}

void
ElasticStretcher::destroy_stretcher ()
{
	delete _stretcher;
	_stretcher = nullptr;
}

/* ========================================================================
 * Reset / reconfigure (non-RT)
 * ====================================================================== */

void
ElasticStretcher::reset ()
{
	_repitch_read_pos  = 0.0;
	_repitch_last_ratio = 1.0;
	_xfade_pending     = false;
	_padding_done      = false;
	_to_drop           = 0;

	for (auto& ch : _xfade_tail) {
		fill (ch.begin (), ch.end (), 0.f);
	}

	if (_stretcher) {
		build_stretcher (); /* easiest way to reset RubberBand state */
	}
}

void
ElasticStretcher::set_warp_map (WarpMap const* warp_map)
{
	if (warp_map) {
		_warp_map_copy = *warp_map;
	} else {
		_warp_map_copy.clear ();
	}
}

void
ElasticStretcher::set_warp_mode (WarpMode mode)
{
	if (mode == _mode) {
		return;
	}
	_mode = mode;
	if (_mode == WarpMode::RePitch) {
		destroy_stretcher ();
	} else {
		build_stretcher ();
	}
	reset ();
}

/* ========================================================================
 * Start-padding (must be called once before the first process())
 * ====================================================================== */

samplecnt_t
ElasticStretcher::fill_start_padding ()
{
	if (!_stretcher || _padding_done) {
		return 0;
	}

	samplecnt_t to_pad;

#ifdef HAVE_RUBBERBAND_3_0_0
	to_pad   = _stretcher->getPreferredStartPad ();
	_to_drop = _stretcher->getStartDelay ();
#else
	to_pad   = _stretcher->getLatency ();
	_to_drop = to_pad;
#endif

	/* Feed zero-valued blocks into the stretcher until the required
	 * padding is fully consumed. */
	vector<vector<float>> silence_bufs (_nchannels, vector<float> (ES_BLOCKSIZE, 0.f));
	vector<float*>        silence_ptrs (_nchannels);
	for (uint32_t c = 0; c < _nchannels; ++c) {
		silence_ptrs[c] = silence_bufs[c].data ();
	}

	samplecnt_t remaining = to_pad;
	while (remaining > 0) {
		samplecnt_t chunk = min (remaining, (samplecnt_t) ES_BLOCKSIZE);
		_stretcher->process (silence_ptrs.data (), (size_t) chunk, false);
		remaining -= chunk;
	}

	_padding_done = true;
	return _to_drop;
}

/* ========================================================================
 * Main process() dispatcher
 * ====================================================================== */

pframes_t
ElasticStretcher::process (double           beat_pos,
                           double           bpm,
                           float* const*    audio_data,
                           samplecnt_t      data_length,
                           samplecnt_t&     read_index,
                           pframes_t        nframes,
                           float**          output,
                           bool             at_end)
{
	if (_mode == WarpMode::RePitch) {
		return process_repitch (beat_pos, bpm, audio_data, data_length,
		                        read_index, nframes, output, at_end);
	}
	return process_stretch (beat_pos, bpm, audio_data, data_length,
	                        read_index, nframes, output, at_end);
}

/* ========================================================================
 * Stretch path (all modes except RePitch)
 * ====================================================================== */

pframes_t
ElasticStretcher::process_stretch (double           beat_pos,
                                   double           bpm,
                                   float* const*    audio_data,
                                   samplecnt_t      data_length,
                                   samplecnt_t&     read_index,
                                   pframes_t        nframes,
                                   float**          output,
                                   bool             at_end)
{
	if (!_stretcher) {
		return 0;
	}

	/* Compute session samples-per-beat */
	const double spb = (bpm > 0.0) ? (_sample_rate * 60.0 / bpm) : _sample_rate;

	/* Compute local stretch ratio from the warp map */
	double ratio = _warp_map_copy.stretch_ratio_at (beat_pos, spb);
	if (ratio <= 0.0 || !std::isfinite (ratio)) {
		ratio = 1.0;
	}

	/* Clamp to sane bounds to avoid extreme stretching artefacts */
	ratio = max (0.1, min (10.0, ratio));

	_stretcher->setTimeRatio (ratio);

	int    avail        = _stretcher->available ();
	if (avail < 0) {
		PBD::error << _("ElasticStretcher: RubberBand stretcher not initialized. Ensure build_stretcher() was called before processing.") << endmsg;
		return 0;
	}

	/* Feed source audio until we have enough output.
	 * Use pre-allocated _in_ptrs / _discard_ptrs — no heap allocation on the RT thread. */
	while ((pframes_t) avail < nframes && read_index < data_length) {
		pframes_t  chunk = (pframes_t) min ((samplecnt_t) ES_BLOCKSIZE,
		                                    data_length - read_index);
		bool       final = (read_index + chunk >= data_length) || at_end;

		for (uint32_t c = 0; c < _nchannels; ++c) {
			_in_ptrs[c] = const_cast<float*> (audio_data[c] + read_index);
		}

		_stretcher->process (_in_ptrs.data (), (size_t) chunk, final);
		read_index += chunk;

		/* Drop RubberBand latency samples on first call after reset */
		if (_to_drop > 0 && (avail = _stretcher->available ()) > 0) {
			samplecnt_t drop = min ((samplecnt_t) avail, _to_drop);
			drop = min (drop, (samplecnt_t) ES_BLOCKSIZE);
			_stretcher->retrieve (_discard_ptrs.data (), (size_t) drop);
			_to_drop -= drop;
		}

		avail = _stretcher->available ();
	}

	/* Retrieve up to nframes of output */
	pframes_t to_retrieve = min ((pframes_t) max (avail, 0), nframes);
	if (to_retrieve == 0) {
		return 0;
	}

	pframes_t retrieved = (pframes_t) _stretcher->retrieve (output, (size_t) to_retrieve);
	return retrieved;
}

/* ========================================================================
 * Re-Pitch path (bypass RubberBand; change playback rate instead)
 * ====================================================================== */

pframes_t
ElasticStretcher::process_repitch (double           beat_pos,
                                   double           bpm,
                                   float* const*    audio_data,
                                   samplecnt_t      data_length,
                                   samplecnt_t&     read_index,
                                   pframes_t        nframes,
                                   float**          output,
                                   bool             /*at_end*/)
{
	/* In Re-Pitch mode we do NOT time-stretch: instead the audio is read
	 * at a rate proportional to the tempo ratio so that pitch changes
	 * together with speed (like vinyl speed-up / slow-down).
	 *
	 * rate = segment_source_spb / session_spb
	 *      = (session_bpm / clip_bpm_at_this_segment)
	 *
	 * Here we use the warp map: the local "clip BPM" for a segment is
	 * the implicit BPM encoded in the ratio
	 *   (sample_delta / beat_delta) / (sample_rate / 60)
	 * which is the inverse of the stretch ratio.  Therefore:
	 *
	 * playback_rate = stretch_ratio(session_spb)   (no inversion needed)
	 */

	const double spb  = (bpm > 0.0) ? (_sample_rate * 60.0 / bpm) : _sample_rate;
	double rate = _warp_map_copy.stretch_ratio_at (beat_pos, spb);
	if (rate <= 0.0 || !std::isfinite (rate)) {
		rate = 1.0;
	}
	rate = max (0.1, min (10.0, rate));

	/* Linear interpolation read-back */
	pframes_t written = 0;

	for (pframes_t n = 0; n < nframes; ++n) {
		samplepos_t idx0 = (samplepos_t) _repitch_read_pos;
		double      frac  = _repitch_read_pos - (double) idx0;

		if (idx0 + 1 >= data_length) {
			break;
		}

		for (uint32_t c = 0; c < _nchannels; ++c) {
			float s0 = audio_data[c][idx0];
			float s1 = audio_data[c][idx0 + 1];
			output[c][n] = s0 + (float) frac * (s1 - s0);
		}

		_repitch_read_pos += rate;
		++written;
	}

	/* Update integer read_index to match fractional position */
	read_index = (samplepos_t) _repitch_read_pos;

	return written;
}

/* ========================================================================
 * Loop crossfade helpers
 * ====================================================================== */

void
ElasticStretcher::capture_xfade_tail (float* const* audio_data,
                                      samplecnt_t   data_length,
                                      samplecnt_t   read_index)
{
	const samplecnt_t tail_start = max ((samplecnt_t) 0,
	                                    data_length - (samplecnt_t) CROSSFADE_LEN);
	const pframes_t   copy_len   = (pframes_t) min ((samplecnt_t) CROSSFADE_LEN,
	                                                 data_length - tail_start);

	for (uint32_t c = 0; c < _nchannels; ++c) {
		memset (_xfade_tail[c].data (), 0, CROSSFADE_LEN * sizeof (float));
		memcpy (_xfade_tail[c].data (),
		        audio_data[c] + tail_start,
		        copy_len * sizeof (float));
	}

	_xfade_pending = true;
}

void
ElasticStretcher::apply_crossfade (float** output, pframes_t nframes)
{
	if (!_xfade_pending) {
		return;
	}

	const pframes_t xlen = min (nframes, CROSSFADE_LEN);

	for (pframes_t i = 0; i < xlen; ++i) {
		const float t        = (float) i / (float) CROSSFADE_LEN;
		const float fade_in  = t * t;            /* quadratic (equal-power approximation) */
		const float fade_out = 1.f - fade_in;

		for (uint32_t c = 0; c < _nchannels; ++c) {
			output[c][i] = output[c][i] * fade_in +
			               _xfade_tail[c][i] * fade_out;
		}
	}

	if (xlen >= CROSSFADE_LEN) {
		_xfade_pending = false;
	}
}
