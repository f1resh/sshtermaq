#pragma once
// A small XML reader for the documents S3 and STS answer with: elements, text, the five entities and character
// references, CDATA, comments and the XML declaration. Namespaces are dropped ("s3:Key" -> "Key"); attributes are
// kept but rarely needed. Not a validating parser; malformed input throws XmlError.
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::s3 {

struct XmlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct XmlNode {
    std::string name;  // local name, without a namespace prefix
    std::vector<std::pair<std::string, std::string>> attributes;
    /** Text directly inside this element (all text pieces joined). */
    std::string text;
    std::vector<XmlNode> children;

    /** First child named `n`, or nullptr. */
    const XmlNode* child(std::string_view n) const;
    /** All children named `n`. */
    std::vector<const XmlNode*> all(std::string_view n) const;
    /** Text of the first child named `n` ("" when there is none). */
    std::string childText(std::string_view n) const;
};

/** Parses a whole document and returns its root element. Limits: 64 levels, `maxBytes` of input. */
XmlNode parseXml(std::string_view doc, size_t maxBytes = 64 * 1024 * 1024);

/** Escapes text for an XML element or attribute value. */
std::string xmlEscape(std::string_view s);

}  // namespace st::s3
