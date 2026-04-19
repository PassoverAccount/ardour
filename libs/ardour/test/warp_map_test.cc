/*
 * Unit tests for WarpMap serialization, ratio calculations, and marker operations.
 */

#include <cmath>
#include "warp_map_test.h"
#include "ardour/warp_marker.h"
#include "pbd/xml++.h"

CPPUNIT_TEST_SUITE_REGISTRATION (WarpMapTest);

using namespace ARDOUR;

void
WarpMapTest::addRemoveTest ()
{
	WarpMap wm;
	CPPUNIT_ASSERT (wm.empty ());
	CPPUNIT_ASSERT_EQUAL ((size_t) 0, wm.size ());

	wm.add_marker (0, 0.0);
	CPPUNIT_ASSERT_EQUAL ((size_t) 1, wm.size ());

	wm.add_marker (44100, 1.0);
	CPPUNIT_ASSERT_EQUAL ((size_t) 2, wm.size ());

	wm.add_marker (88200, 2.0);
	CPPUNIT_ASSERT_EQUAL ((size_t) 3, wm.size ());

	/* Markers should be sorted by beat_pos */
	auto const& markers = wm.markers ();
	CPPUNIT_ASSERT (markers[0].beat_pos < markers[1].beat_pos);
	CPPUNIT_ASSERT (markers[1].beat_pos < markers[2].beat_pos);

	/* Remove middle marker */
	wm.remove_marker (1.0);
	CPPUNIT_ASSERT_EQUAL ((size_t) 2, wm.size ());

	/* Verify remaining markers */
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.0, wm.markers ()[0].beat_pos, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (2.0, wm.markers ()[1].beat_pos, 1e-9);

	/* Adding marker at existing beat_pos updates sample_pos */
	wm.add_marker (99999, 0.0);
	CPPUNIT_ASSERT_EQUAL ((size_t) 2, wm.size ());
	CPPUNIT_ASSERT_EQUAL ((samplepos_t) 99999, wm.markers ()[0].sample_pos);

	/* Clear */
	wm.clear ();
	CPPUNIT_ASSERT (wm.empty ());
}

void
WarpMapTest::stretchRatioTest ()
{
	/* Setup: 48kHz, 120 BPM → spb = 48000 * 60 / 120 = 24000 */
	const double spb = 24000.0;

	WarpMap wm;

	/* With fewer than 2 markers, ratio should be 1.0 */
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, wm.stretch_ratio_at (0.0, spb), 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, wm.stretch_ratio_at (1.0, spb), 1e-9);

	wm.add_marker (0, 0.0);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, wm.stretch_ratio_at (0.5, spb), 1e-9);

	/* Two markers: 0 samples @ beat 0, 24000 samples @ beat 1.
	 * ratio = (1.0 * 24000) / 24000 = 1.0 */
	wm.add_marker (24000, 1.0);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, wm.stretch_ratio_at (0.5, spb), 1e-9);

	/* Three markers: compressed second segment.
	 * Segment 0→1: (1.0 * 24000) / 24000 = 1.0
	 * Segment 1→2: marker at sample 36000, beat 2.0
	 *   ratio = (1.0 * 24000) / (36000 - 24000) = 24000 / 12000 = 2.0 */
	wm.add_marker (36000, 2.0);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, wm.stretch_ratio_at (0.5, spb), 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (2.0, wm.stretch_ratio_at (1.5, spb), 1e-9);
}

void
WarpMapTest::sourceSampleTest ()
{
	WarpMap wm;

	/* With < 2 markers, source_sample_at returns 0 */
	CPPUNIT_ASSERT_EQUAL ((samplepos_t) 0, wm.source_sample_at (1.0));

	wm.add_marker (0, 0.0);
	wm.add_marker (48000, 2.0);

	/* Linear interpolation: beat 1.0 should map to sample 24000 */
	CPPUNIT_ASSERT_EQUAL ((samplepos_t) 24000, wm.source_sample_at (1.0));

	/* Beat 0.5 → sample 12000 */
	CPPUNIT_ASSERT_EQUAL ((samplepos_t) 12000, wm.source_sample_at (0.5));

	/* Beat 2.0 → sample 48000 */
	CPPUNIT_ASSERT_EQUAL ((samplepos_t) 48000, wm.source_sample_at (2.0));
}

void
WarpMapTest::serializationTest ()
{
	WarpMap original;
	original.add_marker (0, 0.0);
	original.add_marker (22050, 1.0);
	original.add_marker (44100, 2.5);
	original.add_marker (88200, 4.0);

	/* Serialize to XML */
	XMLNode& xml = original.get_state ();
	CPPUNIT_ASSERT_EQUAL (std::string ("WarpMap"), xml.name ());

	/* Deserialize into a new WarpMap */
	WarpMap loaded;
	CPPUNIT_ASSERT_EQUAL (0, loaded.set_state (xml, 0));
	CPPUNIT_ASSERT_EQUAL (original.size (), loaded.size ());

	/* Verify all markers match */
	for (size_t i = 0; i < original.size (); ++i) {
		CPPUNIT_ASSERT_EQUAL (original.markers ()[i].sample_pos, loaded.markers ()[i].sample_pos);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (original.markers ()[i].beat_pos, loaded.markers ()[i].beat_pos, 1e-9);
	}

	/* XMLNode& from get_state() is heap-allocated; caller owns it */
	delete &xml;
}

void
WarpMapTest::segmentLookupTest ()
{
	WarpMap wm;

	/* Empty map → segment -1 */
	CPPUNIT_ASSERT_EQUAL (-1, wm.segment_for_beat (0.5));

	wm.add_marker (0, 0.0);

	/* Single marker → -1 (need at least 2) */
	CPPUNIT_ASSERT_EQUAL (-1, wm.segment_for_beat (0.5));

	wm.add_marker (44100, 1.0);
	wm.add_marker (88200, 2.0);

	/* Beat 0.5 → segment 0 (between marker 0 and marker 1) */
	CPPUNIT_ASSERT_EQUAL (0, wm.segment_for_beat (0.5));

	/* Beat 1.5 → segment 1 (between marker 1 and marker 2) */
	CPPUNIT_ASSERT_EQUAL (1, wm.segment_for_beat (1.5));

	/* Beat exactly at marker 1 → segment 1 (marker[1].beat_pos ≤ beat_pos < marker[2].beat_pos) */
	CPPUNIT_ASSERT_EQUAL (1, wm.segment_for_beat (1.0));

	/* Beat before first marker → -1 */
	CPPUNIT_ASSERT_EQUAL (-1, wm.segment_for_beat (-0.5));
}
