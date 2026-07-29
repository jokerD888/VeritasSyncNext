#include "engine/sync/resume_request.h"
#include "tests/test_framework.h"

VSYNC_TEST(ResumeRequestCompressesCompletedBitmapIntoMissingRanges) {
  const auto ranges = veritassync::sync::MissingChunkRanges(10, {8, 0, 1, 4, 4, 5});
  VSYNC_CHECK(ranges.size() == 3);
  VSYNC_CHECK(ranges[0].first_chunk == 2 && ranges[0].chunk_count == 2);
  VSYNC_CHECK(ranges[1].first_chunk == 6 && ranges[1].chunk_count == 2);
  VSYNC_CHECK(ranges[2].first_chunk == 9 && ranges[2].chunk_count == 1);
  VSYNC_CHECK_THROWS(veritassync::sync::MissingChunkRanges(3, {3}));
}
