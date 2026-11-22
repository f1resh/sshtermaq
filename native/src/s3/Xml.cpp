#include "s3/Xml.h"

#include <charconv>
#include <cstdint>

namespace st::s3 {

namespace {

constexpr int kMaxDepth = 64;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isNameChar(char c) { return !isSpace(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '"' && c != '\''; }

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) throw XmlError("invalid character reference");
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/** Decodes entities and character references of a text or attribute value. */
void appendDecoded(std::string& out, std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t amp = s.find('&', i);
        if (amp == std::string_view::npos) {
            out.append(s.substr(i));
            return;
        }
        out.append(s.substr(i, amp - i));
        const size_t semi = s.find(';', amp);
        if (semi == std::string_view::npos || semi - amp > 12) throw XmlError("unterminated entity");
        const std::string_view e = s.substr(amp + 1, semi - amp - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() > 1 && e[0] == '#') {
            const bool hex = e[1] == 'x' || e[1] == 'X';
            const std::string_view digits = e.substr(hex ? 2 : 1);
            uint32_t cp = 0;
            auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            if (ec != std::errc() || p != digits.data() + digits.size() || digits.empty())
                throw XmlError("invalid character reference");
            appendUtf8(out, cp);
        } else {
            throw XmlError("unknown entity &" + std::string(e) + ";");
        }
        i = semi + 1;
    }
}

std::string_view localName(std::string_view qname) {
    const size_t colon = qname.find(':');
    return colon == std::string_view::npos ? qname : qname.substr(colon + 1);
}

class Parser {
public:
    explicit Parser(std::string_view d) : d_(d) {}

    XmlNode document() {
        skipMisc();
        if (!peek("<")) throw XmlError("no root element");
        XmlNode root = element(0);
        skipMisc();
        if (i_ != d_.size()) throw XmlError("content after the root element");
        return root;
    }

private:
    bool peek(std::string_view s) const { return d_.substr(i_, s.size()) == s; }

    void expect(std::string_view s) {
        if (!peek(s)) throw XmlError("expected '" + std::string(s) + "'");
        i_ += s.size();
    }

    void skipSpace() {
        while (i_ < d_.size() && isSpace(d_[i_])) ++i_;
    }

    void skipUntil(std::string_view end) {
        const size_t p = d_.find(end, i_);
        if (p == std::string_view::npos) throw XmlError("unterminated '" + std::string(end) + "'");
        i_ = p + end.size();
    }

    /** Whitespace, comments, processing instructions and a DOCTYPE outside the root. */
    void skipMisc() {
        for (;;) {
            skipSpace();
            if (peek("<?")) skipUntil("?>");
            else if (peek("<!--")) skipUntil("-->");
            else if (peek("<!DOCTYPE")) skipUntil(">");
            else return;
        }
    }

    std::string_view name() {
        const size_t start = i_;
        while (i_ < d_.size() && isNameChar(d_[i_])) ++i_;
        if (i_ == start) throw XmlError("expected a name");
        return d_.substr(start, i_ - start);
    }

    XmlNode element(int depth) {
        if (depth >= kMaxDepth) throw XmlError("elements nested too deeply");
        expect("<");
        const std::string_view qname = name();
        XmlNode n;
        n.name = std::string(localName(qname));
        for (;;) {
            skipSpace();
            if (peek("/>")) {
                i_ += 2;
                return n;
            }
            if (peek(">")) {
                ++i_;
                break;
            }
            const std::string_view an = name();
            skipSpace();
            expect("=");
            skipSpace();
            if (i_ >= d_.size() || (d_[i_] != '"' && d_[i_] != '\'')) throw XmlError("expected a quoted value");
            const char q = d_[i_++];
            const size_t end = d_.find(q, i_);
            if (end == std::string_view::npos) throw XmlError("unterminated attribute value");
            std::string v;
            appendDecoded(v, d_.substr(i_, end - i_));
            i_ = end + 1;
            n.attributes.emplace_back(std::string(an), std::move(v));
        }
        for (;;) {
            if (i_ >= d_.size()) throw XmlError("unterminated element <" + std::string(qname) + ">");
            if (peek("</")) {
                i_ += 2;
                const std::string_view closing = name();
                if (closing != qname) throw XmlError("mismatched </" + std::string(closing) + ">");
                skipSpace();
                expect(">");
                return n;
            }
            if (peek("<!--")) {
                skipUntil("-->");
            } else if (peek("<![CDATA[")) {
                i_ += 9;
                const size_t end = d_.find("]]>", i_);
                if (end == std::string_view::npos) throw XmlError("unterminated CDATA");
                n.text.append(d_.substr(i_, end - i_));
                i_ = end + 3;
            } else if (peek("<?")) {
                skipUntil("?>");
            } else if (peek("<")) {
                n.children.push_back(element(depth + 1));
            } else {
                const size_t lt = d_.find('<', i_);
                const size_t end = lt == std::string_view::npos ? d_.size() : lt;
                appendDecoded(n.text, d_.substr(i_, end - i_));
                i_ = end;
            }
        }
    }

    std::string_view d_;
    size_t i_ = 0;
};

}  // namespace

const XmlNode* XmlNode::child(std::string_view n) const {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::all(std::string_view n) const {
    std::vector<const XmlNode*> out;
    for (const auto& c : children)
        if (c.name == n) out.push_back(&c);
    return out;
}

std::string XmlNode::childText(std::string_view n) const {
    const XmlNode* c = child(n);
    return c ? c->text : std::string();
}

XmlNode parseXml(std::string_view doc, size_t maxBytes) {
    if (doc.size() > maxBytes) throw XmlError("response too large");
    if (doc.substr(0, 3) == "\xEF\xBB\xBF") doc.remove_prefix(3);
    return Parser(doc).document();
}

std::string xmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace st::s3
