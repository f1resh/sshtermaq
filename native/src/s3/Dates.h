#pragma once
// The date formats of S3 and AWS: ISO 8601 ("2009-10-12T17:50:30.000Z", "2026-11-21T10:00:00+00:00") and HTTP dates
// ("Wed, 12 Oct 2009 17:50:00 GMT").
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace st::s3 {

/** Unix milliseconds; nullopt when the text is not such a date. */
std::optional<int64_t> parseIso8601(std::string_view s);
std::optional<int64_t> parseHttpDate(std::string_view s);

/** Days since 1970-01-01 for a civil date, and back. */
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d);

/** "2026-11-21T10:00:00Z" for Unix seconds. */
std::string formatIso8601(int64_t unixSeconds);

}  // namespace st::s3
