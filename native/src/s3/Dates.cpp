#include "s3/Dates.h"

#include "s3/SigV4.h"

#include <cctype>
#include <charconv>

namespace st::s3 {

namespace {

bool num(std::string_view s, size_t pos, size_t len, int& out) {
    if (pos + len > s.size()) return false;
    for (size_t i = pos; i < pos + len; ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    auto [p, ec] = std::from_chars(s.data() + pos, s.data() + pos + len, out);
    return ec == std::errc();
}

bool valid(int y, int mo, int d, int h, int mi, int sec) {
    return y >= 1970 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59 &&
           sec >= 0 && sec <= 60;
}

int64_t toMs(int y, int mo, int d, int h, int mi, int sec, int ms) {
    const int64_t days = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ((days * 86400 + h * 3600 + mi * 60 + sec) * 1000) + ms;
}

}  // namespace

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::optional<int64_t> parseIso8601(std::string_view s) {
    int y, mo, d, h = 0, mi = 0, sec = 0, ms = 0;
    if (!num(s, 0, 4, y) || s.size() < 10 || s[4] != '-' || !num(s, 5, 2, mo) || s[7] != '-' || !num(s, 8, 2, d))
        return std::nullopt;
    size_t i = 10;
    if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        if (!num(s, i + 1, 2, h) || s.size() < i + 9 || s[i + 3] != ':' || !num(s, i + 4, 2, mi) || s[i + 6] != ':' ||
            !num(s, i + 7, 2, sec))
            return std::nullopt;
        i += 9;
        if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
            ++i;
            int digits = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                if (digits < 3) ms = ms * 10 + (s[i] - '0');
                ++digits;
                ++i;
            }
            for (; digits < 3; ++digits) ms *= 10;
        }
    }
    if (!valid(y, mo, d, h, mi, sec)) return std::nullopt;
    int64_t offsetMin = 0;
    if (i < s.size()) {
        if (s[i] == 'Z' || s[i] == 'z') {
            ++i;
        } else if (s[i] == '+' || s[i] == '-') {
            int oh = 0, om = 0;
            const bool neg = s[i] == '-';
            if (!num(s, i + 1, 2, oh)) return std::nullopt;
            size_t j = i + 3;
            if (j < s.size() && s[j] == ':') ++j;
            if (!num(s, j, 2, om)) return std::nullopt;
            offsetMin = (neg ? -1 : 1) * (oh * 60 + om);
            i = j + 2;
        }
    }
    if (i != s.size()) return std::nullopt;
    return toMs(y, mo, d, h, mi, sec, ms) - offsetMin * 60000;
}

std::optional<int64_t> parseHttpDate(std::string_view s) {
    // "Wed, 12 Oct 2009 17:50:00 GMT"
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const size_t comma = s.find(", ");
    if (comma == std::string_view::npos) return std::nullopt;
    s.remove_prefix(comma + 2);
    int d, y, h, mi, sec;
    if (s.size() < 20 || !num(s, 0, 2, d) || s[2] != ' ' || s[6] != ' ' || !num(s, 7, 4, y) || s[11] != ' ' ||
        !num(s, 12, 2, h) || s[14] != ':' || !num(s, 15, 2, mi) || s[17] != ':' || !num(s, 18, 2, sec))
        return std::nullopt;
    int mo = 0;
    for (int k = 0; k < 12; ++k)
        if (s.substr(3, 3) == kMonths[k]) mo = k + 1;
    if (!mo || !valid(y, mo, d, h, mi, sec)) return std::nullopt;
    return toMs(y, mo, d, h, mi, sec, 0);
}

std::string formatIso8601(int64_t unixSeconds) {
    const std::string a = amzDate(unixSeconds);  // 20261121T100000Z
    return a.substr(0, 4) + "-" + a.substr(4, 2) + "-" + a.substr(6, 2) + "T" + a.substr(9, 2) + ":" + a.substr(11, 2) +
           ":" + a.substr(13, 2) + "Z";
}

}  // namespace st::s3
