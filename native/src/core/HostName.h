#pragma once
// Host names as the user may give them: the one check for links, the command line and connection requests. The
// same rule as the UI's isValidHost (ui/src/components/dialogs/session/sessionModel.ts); both are tested against
// native/tests/fixtures/host-names.json.
#include <string_view>

namespace st {

/** Dotted-quad IPv4 ("192.0.2.1"; no leading zeros beyond a single digit, each part 0-255). */
bool isIPv4(std::string_view s);
/** IPv6 literal, brackets optional, with an optional zone ("fe80::1%eth0") or an embedded IPv4 tail. */
bool isIPv6(std::string_view s);
/** A DNS name (labels of letters, digits, '_' and '-', not starting with '-', 63 characters each, 253 in all, an
 *  optional final dot), an IPv4 address or an IPv6 literal. Nothing else: no spaces, quotes or shell characters. */
bool isValidHostName(std::string_view s);

}  // namespace st
