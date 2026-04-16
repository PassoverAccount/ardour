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

#include <cstring>

#include "ardour/warp_mode.h"

using namespace RubberBand;

namespace ARDOUR {

RubberBandStretcher::Options
rb_options_for_warp_mode (WarpMode mode)
{
	using Opt = RubberBandStretcher;

	switch (mode) {
	case WarpMode::Beats:
		/* Crisp transient preservation — best for drums / percussion */
		return Opt::Option (Opt::OptionProcessRealTime |
		                    Opt::OptionTransientsCrisp);

	case WarpMode::Tones:
		/* Phase-coherent, good for monophonic melodic material */
		return Opt::Option (Opt::OptionProcessRealTime |
		                    Opt::OptionTransientsSmooth |
		                    Opt::OptionPhaseIndependent |
		                    Opt::OptionPitchHighQuality);

	case WarpMode::Texture:
		/* Long window, smooth transients — ambient / pads */
		return Opt::Option (Opt::OptionProcessRealTime |
		                    Opt::OptionTransientsSmooth |
		                    Opt::OptionWindowLong);

	case WarpMode::RePitch:
		/* No time-stretch: caller bypasses RubberBand entirely */
		return Opt::Option (0);

	case WarpMode::Complex:
		/* Balanced trade-off for polyphonic material */
		return Opt::Option (Opt::OptionProcessRealTime |
		                    Opt::OptionTransientsMixed);

	case WarpMode::ComplexPro:
		/* Highest quality: long window + formant preservation */
		return Opt::Option (Opt::OptionProcessRealTime |
		                    Opt::OptionTransientsMixed |
		                    Opt::OptionWindowLong |
		                    Opt::OptionFormantPreserved);
	}

	/* Fallback */
	return Opt::Option (Opt::OptionProcessRealTime |
	                    Opt::OptionTransientsMixed);
}

const char*
warp_mode_to_string (WarpMode mode)
{
	switch (mode) {
	case WarpMode::Beats:      return "Beats";
	case WarpMode::Tones:      return "Tones";
	case WarpMode::Texture:    return "Texture";
	case WarpMode::RePitch:    return "Re-Pitch";
	case WarpMode::Complex:    return "Complex";
	case WarpMode::ComplexPro: return "Complex Pro";
	}
	return "Complex";
}

bool
warp_mode_from_string (const char* s, WarpMode& mode)
{
	if (!s) { return false; }
	if (strcmp (s, "Beats")       == 0) { mode = WarpMode::Beats;      return true; }
	if (strcmp (s, "Tones")       == 0) { mode = WarpMode::Tones;      return true; }
	if (strcmp (s, "Texture")     == 0) { mode = WarpMode::Texture;    return true; }
	if (strcmp (s, "Re-Pitch")    == 0) { mode = WarpMode::RePitch;    return true; }
	if (strcmp (s, "Complex")     == 0) { mode = WarpMode::Complex;    return true; }
	if (strcmp (s, "Complex Pro") == 0) { mode = WarpMode::ComplexPro; return true; }
	return false;
}

} /* namespace ARDOUR */
