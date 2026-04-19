#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

class WarpMapTest : public CppUnit::TestFixture
{
	CPPUNIT_TEST_SUITE (WarpMapTest);
	CPPUNIT_TEST (addRemoveTest);
	CPPUNIT_TEST (stretchRatioTest);
	CPPUNIT_TEST (sourceSampleTest);
	CPPUNIT_TEST (serializationTest);
	CPPUNIT_TEST (segmentLookupTest);
	CPPUNIT_TEST_SUITE_END ();

public:
	void setUp () {}
	void tearDown () {}

	void addRemoveTest ();
	void stretchRatioTest ();
	void sourceSampleTest ();
	void serializationTest ();
	void segmentLookupTest ();
};
