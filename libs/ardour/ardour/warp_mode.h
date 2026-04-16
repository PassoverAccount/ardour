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

#include <rubberband/RubberBandStretcher.h>

#include "ardour/libardour_visibility.h"

namespace ARDOUR {

/** Ableton-inspired warp modes that map onto RubberBand stretcher options.
 *
 *  | Mode       | Use case                                    |
 *  |------------|---------------------------------------------|
 *  | Beats      | Drums, percussive loops                     |
 *  | Tones      | Monophonic melodic material                 |
 *  | Texture    | Ambient, sustained, blurred-transient sounds|
 *  | RePitch    | Bypass time-stretch; pitch follows tempo    |
 *  | Complex    | Mixed polyphonic material                   |
 *  | ComplexPro | Highest quality; formant-preserved, slowest |
 */
enum class WarpMode {
	Beats,      ///< Crisp transient preservation, short windows
	Tones,      ///< Phase-coherent, independent channels
	Texture,    ///< Smooth transients, long analysis window
	RePitch,    ///< No time-stretch: only change playback rate
	Complex,    ///< Balanced transient/pitch trade-off
	ComplexPro, ///< Long window + formant preservation
};

/** Return the RubberBand Options bitmask appropriate for @p mode.
 *  Always includes OptionProcessRealTime.
 *  For WarpMode::RePitch returns 0 — the caller should bypass RubberBand
 *  and change the playback read-increment instead.
 */
LIBARDOUR_API
RubberBand::RubberBandStretcher::Options rb_options_for_warp_mode (WarpMode mode);

/** Human-readable name for @p mode (for menus / serialisation). */
LIBARDOUR_API const char* warp_mode_to_string (WarpMode mode);

/** Parse a string returned by warp_mode_to_string(). */
LIBARDOUR_API bool warp_mode_from_string (const char* s, WarpMode& mode);

} /* namespace ARDOUR */
