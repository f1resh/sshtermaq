#include "s3/Xml.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace st::s3;

TEST_CASE("xml: an S3 ListObjectsV2 answer") {
    const std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>bucket</Name>
  <Prefix>photos/</Prefix>
  <KeyCount>2</KeyCount>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken>
  <Contents>
    <Key>photos/a &amp; b.jpg</Key>
    <LastModified>2009-10-12T17:50:30.000Z</LastModified>
    <ETag>&quot;fba9dede5f27731c9771645a39863328&quot;</ETag>
    <Size>434234</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <!-- a comment -->
  <CommonPrefixes><Prefix>photos/2006/</Prefix></CommonPrefixes>
  <CommonPrefixes><Prefix>photos/&#x65E5;&#26412;/</Prefix></CommonPrefixes>
</ListBucketResult>)";
    const XmlNode root = parseXml(doc);
    CHECK(root.name == "ListBucketResult");
    CHECK(root.attributes.size() == 1);
    CHECK(root.childText("IsTruncated") == "true");
    CHECK(root.childText("NextContinuationToken") == "1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=");
    const auto contents = root.all("Contents");
    REQUIRE(contents.size() == 1);
    CHECK(contents[0]->childText("Key") == "photos/a & b.jpg");
    CHECK(contents[0]->childText("ETag") == "\"fba9dede5f27731c9771645a39863328\"");
    CHECK(contents[0]->childText("Size") == "434234");
    const auto prefixes = root.all("CommonPrefixes");
    REQUIRE(prefixes.size() == 2);
    CHECK(prefixes[1]->childText("Prefix") == "photos/\xE6\x97\xA5\xE6\x9C\xAC/");
    CHECK(root.child("Missing") == nullptr);
    CHECK(root.childText("Missing").empty());
}

TEST_CASE("xml: namespaces, empty elements, CDATA, BOM") {
    const XmlNode r = parseXml("\xEF\xBB\xBF<a:Root xmlns:a='x'><a:Empty/><b:T>x<![CDATA[<&>]]>y</b:T></a:Root>");
    CHECK(r.name == "Root");
    REQUIRE(r.child("Empty"));
    CHECK(r.child("Empty")->text.empty());
    CHECK(r.childText("T") == "x<&>y");
}

TEST_CASE("xml: an S3 error document") {
    const XmlNode e = parseXml(R"(<?xml version="1.0" encoding="UTF-8"?>
<Error><Code>SignatureDoesNotMatch</Code><Message>The request signature we calculated does not match</Message>
<RequestId>4442587FB7D0A2F9</RequestId></Error>)");
    CHECK(e.name == "Error");
    CHECK(e.childText("Code") == "SignatureDoesNotMatch");
}

TEST_CASE("xml: malformed documents are refused") {
    CHECK_THROWS_AS(parseXml(""), XmlError);
    CHECK_THROWS_AS(parseXml("<a>"), XmlError);
    CHECK_THROWS_AS(parseXml("<a></b>"), XmlError);
    CHECK_THROWS_AS(parseXml("<a>&bogus;</a>"), XmlError);
    CHECK_THROWS_AS(parseXml("<a>&#0;</a>"), XmlError);
    CHECK_THROWS_AS(parseXml("<a x=1/>"), XmlError);
    CHECK_THROWS_AS(parseXml("<a/><b/>"), XmlError);
    CHECK_THROWS_AS(parseXml("<a/>", 2), XmlError);
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "<x>";
    for (int i = 0; i < 100; ++i) deep += "</x>";
    CHECK_THROWS_AS(parseXml(deep), XmlError);
}

TEST_CASE("xml: escaping") {
    CHECK(xmlEscape("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&apos;");
    CHECK(parseXml("<k>" + xmlEscape("x & <y>") + "</k>").text == "x & <y>");
}
