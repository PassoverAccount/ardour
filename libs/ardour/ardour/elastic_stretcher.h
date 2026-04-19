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

#include <cstdint>
#include <memory>
#include <vector>

#include <rubberband/RubberBandStretcher.h>

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"
#include "ardour/warp_marker.h"
#include "ardour/warp_mode.h"

namespace ARDOUR {

/** Real-time non-uniform time-stretcher for the Cue/Trigger system.
 *
 *  ElasticStretcher wraps a RubberBand::RubberBandStretcher and drives it
 *  with a WarpMap so that each segment between adjacent warp markers gets
 *  its own local time-ratio computed as:
 *
 *    ratio_i = ((m_{i+1} − m_i) × session_spb) / (s_{i+1} − s_i)
 *
 *  where session_spb = sample_rate × 60 / session_bpm.
 *
 *  For WarpMode::RePitch RubberBand is bypassed entirely: the audio data is
 *  read back at a rate that changes its pitch proportionally to the tempo
 *  ratio, using linear interpolation.
 *
 *  Thread safety
 *  -------------
 *  process() is called from the real-time audio thread.  The caller must
 *  ensure that reset() or set_warp_map() is not called concurrently (they
 *  must only be called from a non-RT thread with appropriate serialisation).
 *  The recommended pattern is an atomic pointer swap: the UI/worker thread
 *  builds a new ElasticStretcher, and an std::atomic<ElasticStretcher*>
 *  inside AudioTrigger is swapped so that the RT thread picks up the new
 *  object on the next process cycle.
 */
class LIBARDOUR_API ElasticStretcher {
  public:
	/** Construct a stretcher for @p nchannels channels at @p sample_rate Hz.
	 *  @param sample_rate  Session sample rate.
	 *  @param nchannels    Number of audio channels to process.
	 *  @param mode         Warp mode; determines RubberBand options.
	 *  @param warp_map     The warp marker map (may be nullptr or empty for
	 *                      uniform stretch identical to the legacy path).
	 */
	ElasticStretcher (double        sample_rate,
	                  uint32_t      nchannels,
	                  WarpMode      mode,
	                  WarpMap const* warp_map);

	~ElasticStretcher ();

	/** Reset internal state (call after a discontinuous playback position
	 *  change, e.g. loop wrap-around or retrigger).  Not real-time safe. */
	void reset ();

	/** Replace the warp map.  Must be called from a non-RT context before
	 *  the next process() call on the same object. */
	void set_warp_map (WarpMap const* warp_map);

	/** Replace the warp mode (rebuilds the RubberBand stretcher).
	 *  Not real-time safe. */
	void set_warp_mode (WarpMode mode);

	/* ------------------------------------------------------------------ */
	/** Process @p nframes of output.
	 *
	 *  @param beat_pos        Current playback position in beats (output timeline).
	 *  @param bpm             Current session BPM (used to compute spb).
	 *  @param audio_data      Pointer-array (one float* per channel) pointing at
	 *                         the start of the complete clip audio data buffer.
	 *  @param data_length     Length of each channel buffer in samples.
	 *  @param read_index      [in/out] Current read position in the source data.
	 *                         Incremented by source samples consumed.
	 *  @param nframes         Number of output samples requested.
	 *  @param output          Pointer-array (one float* per channel) for output.
	 *  @param at_end          True when the caller has reached the end of the
	 *                         source data (triggers RubberBand final flush).
	 *  @return                Number of output samples actually written
	 *                         (may be < nframes near end of clip).
	 */
	pframes_t process (double           beat_pos,
	                   double           bpm,
	                   float* const*    audio_data,
	                   samplecnt_t      data_length,
	                   samplecnt_t&     read_index,
	                   pframes_t        nframes,
	                   float**          output,
	                   bool             at_end);

	/** Fill the start-latency padding that RubberBand requires before
	 *  producing useful output.  Must be called once after the first
	 *  setTimeRatio() call (i.e. just before the first process() call).
	 *  Returns the number of output samples that must be dropped afterwards. */
	samplecnt_t fill_start_padding ();

	/** Return the RubberBand stretcher (for callers that need to query
	 *  latency, available samples, etc.) — nullptr for RePitch mode. */
	RubberBand::RubberBandStretcher* stretcher () const { return _stretcher; }

	bool is_repitch () const { return _mode == WarpMode::RePitch; }

	WarpMode warp_mode () const { return _mode; }

  private:
	double      _sample_rate;
	uint32_t    _nchannels;
	WarpMode    _mode;
	WarpMap     _warp_map_copy; /* local copy; safe for RT reads */

	RubberBand::RubberBandStretcher* _stretcher;

	/* RePitch state */
	double      _repitch_read_pos;   /* fractional source sample position */
	double      _repitch_last_ratio; /* cached ratio to detect changes */

	/* Loop-crossfade buffer (small, per-channel) */
	static const pframes_t CROSSFADE_LEN = 256;
	std::vector<std::vector<float>> _xfade_tail; /* [channel][CROSSFADE_LEN] */
	bool _xfade_pending;

	/* Latency book-keeping */
	bool        _padding_done;
	samplecnt_t _to_drop;

	/* Pre-allocated RT scratch — avoids heap allocation on the audio thread */
	std::vector<float*> _in_ptrs;      /* [_nchannels] per-channel input pointers  */
	std::vector<float>  _discard_buf;  /* [_nchannels * ES_BLOCKSIZE] flat buffer  */
	std::vector<float*> _discard_ptrs; /* [_nchannels] pointers into _discard_buf  */

	void build_stretcher ();
	void destroy_stretcher ();

	pframes_t process_stretch  (double beat_pos, double bpm,
	                             float* const* audio_data, samplecnt_t data_length,
	                             samplecnt_t& read_index,
	                             pframes_t nframes, float** output, bool at_end);

	pframes_t process_repitch  (double beat_pos, double bpm,
	                             float* const* audio_data, samplecnt_t data_length,
	                             samplecnt_t& read_index,
	                             pframes_t nframes, float** output, bool at_end);

	void apply_crossfade (float** output, pframes_t nframes);
	void capture_xfade_tail (float* const* audio_data,
	                         samplecnt_t   data_length,
	                         samplecnt_t   read_index);
};

} /* namespace ARDOUR */
