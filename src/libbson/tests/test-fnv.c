/*
 * Created by Evgeni Dobranov on 6/14/18.
 */

#include "bson-fnv.h"
#include "TestSuite.h"

/* REPEAT500 - repeat a string 500 times */
#define R500(x) R100(x)R100(x)R100(x)R100(x)R100(x)
#define R100(x) R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)R10(x)
#define R10(x) x x x x x x x x x x

struct test_vector {
   char *str;         /* start of test vector buffer */
   u_int32_t hash;    /* length of test vector */
};

static void
test_fnv_compare_vals (void)
{
   unsigned i;

   struct test_vector v[] = {
      { "", (u_int32_t) 0x1c9d44 },
      { "a", (u_int32_t) 0x0c29c8 },
      { "b", (u_int32_t) 0x0c2d02 },
      { "c", (u_int32_t) 0x0c2cb4 },
      { "d", (u_int32_t) 0x0c2492 },
      { "e", (u_int32_t) 0x0c2200 },
      { "f", (u_int32_t) 0x0c277a },
      { "fo", (u_int32_t) 0x22e820 },
      { "foo", (u_int32_t) 0xf37e7e },
      { "foob", (u_int32_t) 0x5076d0 },
      { "fooba", (u_int32_t) 0xaaa1b3 },
      { "foobar", (u_int32_t) 0x9cf9d7 },
      { "ch", (u_int32_t) 0x299f11 },
      { "cho", (u_int32_t) 0x85801c },
      { "chon", (u_int32_t) 0x29778b },
      { "chong", (u_int32_t) 0x46b985 },
      { "chongo", (u_int32_t) 0x564ec0 },
      { "chongo ", (u_int32_t) 0xdd5c0c },
      { "chongo w", (u_int32_t) 0x77eded },
      { "chongo wa", (u_int32_t) 0xca9677 },
      { "chongo was", (u_int32_t) 0xeb9b9a },
      { "chongo was ", (u_int32_t) 0xe67a30 },
      { "chongo was h", (u_int32_t) 0xd32f6a },
      { "chongo was he", (u_int32_t) 0x743fc8 },
      { "chongo was her", (u_int32_t) 0x006376 },
      { "chongo was here", (u_int32_t) 0x9c99cb },
      { "chongo was here!", (u_int32_t) 0x8524b9 },
      { "chongo was here!\n", (u_int32_t) 0x993001 },
      { "cu", (u_int32_t) 0x298129 },
      { "cur", (u_int32_t) 0x5637c9 },
      { "curd", (u_int32_t) 0xb9140f },
      { "curds", (u_int32_t) 0x5bf5a7 },
      { "curds ", (u_int32_t) 0xc42805 },
      { "curds a", (u_int32_t) 0xcc0e97 },
      { "curds an", (u_int32_t) 0x3b4c5d },
      { "curds and", (u_int32_t) 0x59f0a7 },
      { "curds and ", (u_int32_t) 0x94de0b },
      { "curds and w", (u_int32_t) 0x5a0a72 },
      { "curds and wh", (u_int32_t) 0xbee56f },
      { "curds and whe", (u_int32_t) 0x8363fd },
      { "curds and whey", (u_int32_t) 0xd5346c },
      { "curds and whey\n", (u_int32_t) 0xa14715 },
      { "hi", (u_int32_t) 0x3af6f2 },
      { "hello", (u_int32_t) 0x9f2ce4 },
      { "\x40\x51\x4e\x44", (u_int32_t) 0x17906a },
      { "\x44\x4e\x51\x40", (u_int32_t) 0x0bfece },
      { "\x40\x51\x4e\x4a", (u_int32_t) 0x178d02 },
      { "\x4a\x4e\x51\x40", (u_int32_t) 0xaddad9 },
      { "\x40\x51\x4e\x54", (u_int32_t) 0x17a9ca },
      { "\x54\x4e\x51\x40", (u_int32_t) 0x2633a1 },
      { "127.0.0.1", (u_int32_t) 0xa3d116 },
      { "127.0.0.2", (u_int32_t) 0xa3cf8c },
      { "127.0.0.3", (u_int32_t) 0xa3cdfe },
      { "64.81.78.68", (u_int32_t) 0x5636ba },
      { "64.81.78.74", (u_int32_t) 0x53e841 },
      { "64.81.78.84", (u_int32_t) 0x5b8948 },
      { "feedface", (u_int32_t) 0x88b139 },
      { "feedfacedaffdeed", (u_int32_t) 0x364109 },
      { "feedfacedeadbeef", (u_int32_t) 0x7604b9 },
      { "line 1\nline 2\nline 3", (u_int32_t) 0xb4eab4 },
      { "chongo <Landon Curt Noll> /\\../\\", (u_int32_t) 0x4e927c },
      { "chongo (Landon Curt Noll) /\\../\\", (u_int32_t) 0x1b25e1 },
      { "Evgeni was here :D", (u_int32_t) 0xebf05e },
      { "http://antwrp.gsfc.nasa.gov/apod/astropix.html", (u_int32_t) 0x524a34 },
      { "http://en.wikipedia.org/wiki/Fowler_Noll_Vo_hash", (u_int32_t) 0x16ef98 },
      { "http://epod.usra.edu/", (u_int32_t) 0x648bd3 },
      { "http://exoplanet.eu/", (u_int32_t) 0xa4bc83 },
      { "http://hvo.wr.usgs.gov/cam3/", (u_int32_t) 0x53ae47 },
      { "http://hvo.wr.usgs.gov/cams/HMcam/", (u_int32_t) 0x302859 },
      { "http://hvo.wr.usgs.gov/kilauea/update/deformation.html", (u_int32_t) 0x6deda7 },
      { "http://hvo.wr.usgs.gov/kilauea/update/images.html", (u_int32_t) 0x36db15 },
      { "http://hvo.wr.usgs.gov/kilauea/update/maps.html", (u_int32_t) 0x9d33fc },
      { "http://hvo.wr.usgs.gov/volcanowatch/current_issue.html", (u_int32_t) 0xbb6ce2 },
      { "http://neo.jpl.nasa.gov/risk/", (u_int32_t) 0xf83893 },
      { "http://norvig.com/21-days.html", (u_int32_t) 0x08bf51 },
      { "http://primes.utm.edu/curios/home.php", (u_int32_t) 0xcc8e5f },
      { "http://slashdot.org/", (u_int32_t) 0xe20f9f },
      { "http://tux.wr.usgs.gov/Maps/155.25-19.5.html", (u_int32_t) 0xe97f2e },
      { "http://volcano.wr.usgs.gov/kilaueastatus.php", (u_int32_t) 0x37b27b },
      { "http://www.avo.alaska.edu/activity/Redoubt.php", (u_int32_t) 0x9e874a },
      { "http://www.dilbert.com/fast/", (u_int32_t) 0xe63f5a },
      { "http://www.fourmilab.ch/gravitation/orbits/", (u_int32_t) 0xb50b11 },
      { "http://www.fpoa.net/", (u_int32_t) 0xd678e6 },
      { "http://www.ioccc.org/index.html", (u_int32_t) 0xd5b723 },
      { "http://www.isthe.com/cgi-bin/number.cgi", (u_int32_t) 0x450bb7 },
      { "http://www.isthe.com/chongo/bio.html", (u_int32_t) 0x72d79d },
      { "http://www.isthe.com/chongo/index.html", (u_int32_t) 0x06679c },
      { "http://www.isthe.com/chongo/src/calc/lucas-calc", (u_int32_t) 0x52e15c },
      { "http://www.isthe.com/chongo/tech/astro/venus2004.html", (u_int32_t) 0x9664f7 },
      { "http://www.isthe.com/chongo/tech/astro/vita.html", (u_int32_t) 0x3258b6 },
      { "http://www.isthe.com/chongo/tech/comp/c/expert.html", (u_int32_t) 0xed6ea7 },
      { "http://www.isthe.com/chongo/tech/comp/calc/index.html", (u_int32_t) 0x7d7ce2 },
      { "http://www.isthe.com/chongo/tech/comp/fnv/index.html", (u_int32_t) 0xc71ba1 },
      { "http://www.isthe.com/chongo/tech/math/number/howhigh.html", (u_int32_t) 0x84f14b },
      { "http://www.isthe.com/chongo/tech/math/number/number.html", (u_int32_t) 0x8ecf2e },
      { "http://www.isthe.com/chongo/tech/math/prime/mersenne.html", (u_int32_t) 0x94f673 },
      { "http://www.isthe.com/chongo/tech/math/prime/mersenne.html#largest", (u_int32_t) 0x970112 },
      { "http://www.lavarnd.org/cgi-bin/corpspeak.cgi", (u_int32_t) 0x6e172a },
      { "http://www.lavarnd.org/cgi-bin/haiku.cgi", (u_int32_t) 0xf8f6e7 },
      { "http://www.lavarnd.org/cgi-bin/rand-none.cgi", (u_int32_t) 0xf58843 },
      { "http://www.lavarnd.org/cgi-bin/randdist.cgi", (u_int32_t) 0x17b6b2 },
      { "http://www.lavarnd.org/index.html", (u_int32_t) 0xad4cfb },
      { "http://www.lavarnd.org/what/nist-test.html", (u_int32_t) 0x256811 },
      { "http://www.macosxhints.com/", (u_int32_t) 0xb18dd8 },
      { "http://www.mellis.com/", (u_int32_t) 0x61c153 },
      { "http://www.nature.nps.gov/air/webcams/parks/havoso2alert/havoalert.cfm", (u_int32_t) 0x47d20d },
      { "http://www.nature.nps.gov/air/webcams/parks/havoso2alert/timelines_24.cfm", (u_int32_t) 0x8b689f },
      { "http://www.paulnoll.com/", (u_int32_t) 0xd2a40b },
      { "http://www.pepysdiary.com/", (u_int32_t) 0x549b0a },
      { "http://www.sciencenews.org/index/home/activity/view", (u_int32_t) 0xe1b55b },
      { "http://www.skyandtelescope.com/", (u_int32_t) 0x0cd3d1 },
      { "http://www.sput.nl/~rob/sirius.html", (u_int32_t) 0x471605 },
      { "http://www.systemexperts.com/", (u_int32_t) 0x5eef10 },
      { "http://www.tq-international.com/phpBB3/index.php", (u_int32_t) 0xed3629 },
      { "http://www.travelquesttours.com/index.htm", (u_int32_t) 0x624952 },
      { "http://www.wunderground.com/global/stations/89606.html", (u_int32_t) 0x9b8688 },
      { R10("21701"), (u_int32_t) 0x15e25f },
      { R10("M21701"), (u_int32_t) 0xa98d05 },
      { R10("2^21701-1"), (u_int32_t) 0xdf8bcc },
      { R10("\x54\xc5"), (u_int32_t) 0x1e9051 },
      { R10("\xc5\x54"), (u_int32_t) 0x3f70db },
      { R10("23209"), (u_int32_t) 0x95aedb },
      { R10("M23209"), (u_int32_t) 0xa7f7d7 },
      { R10("2^23209-1"), (u_int32_t) 0x3bc660 },
      { R10("\x5a\xa9"), (u_int32_t) 0x610967 },
      { R10("\xa9\x5a"), (u_int32_t) 0x157785 },
      { R10("391581216093"), (u_int32_t) 0x2b2800 },
      { R10("391581*2^216093-1"), (u_int32_t) 0x8239ef },
      { R10("\x05\xf9\x9d\x03\x4c\x81"), (u_int32_t) 0x5869f5 },
      { R10("FEDCBA9876543210"), (u_int32_t) 0x415c76 },
      { R10("\xfe\xdc\xba\x98\x76\x54\x32\x10"), (u_int32_t) 0xe4ff6f },
      { R10("EFCDAB8967452301"), (u_int32_t) 0xb7977d },
      { R10("\xef\xcd\xab\x89\x67\x45\x23\x01"), (u_int32_t) 0xa43a7b },
      { R10("0123456789ABCDEF"), (u_int32_t) 0xb3be1e },
      { R10("\x01\x23\x45\x67\x89\xab\xcd\xef"), (u_int32_t) 0x777aaf },
      { R10("1032547698BADCFE"), (u_int32_t) 0x21c38a },
      { R10("\x10\x32\x54\x76\x98\xba\xdc\xfe"), (u_int32_t) 0x9d0839 },
      { R500("\x07"), (u_int32_t) 0xa27250 },
      { R500("~"), (u_int32_t) 0xc5c656 },
      { R500("\x7f"), (u_int32_t) 0x3b0800 },
      { NULL, 0 } /* MUST BE LAST */
   };

   /* Assuming 140 tests entries available above */
   for (i = 0; i < 140; ++i) {
      if (fnv_24a_str (v[i].str) != v[i].hash) {
         fprintf (stderr, "(%u) String:   %s\n", i, v[i].str);
         fprintf (stderr, "Computed hash: 0x%x\n",
                  fnv_24a_str (v[i].str));
         fprintf (stderr, "Actual hash:   0x%x\n\n", v[i].hash);
      }
   }
}

void
test_fnv_install (TestSuite *suite)
{
   TestSuite_Add (suite, "/fnv/compare", test_fnv_compare_vals);
}
