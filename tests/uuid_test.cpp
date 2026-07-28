#include "engine/common/uuid.h"
#include "tests/test_framework.h"

VSYNC_TEST(UuidGeneratorProducesRandomVersionFourFormat) {
  const auto value = veritassync::common::NewUuidV4();
  VSYNC_CHECK(value.size() == 36);
  VSYNC_CHECK(value[8] == '-');
  VSYNC_CHECK(value[13] == '-');
  VSYNC_CHECK(value[14] == '4');
  VSYNC_CHECK(value[18] == '-');
  VSYNC_CHECK(value[23] == '-');
  VSYNC_CHECK(value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}
