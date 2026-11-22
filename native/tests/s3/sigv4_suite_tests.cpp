// AWS Signature Version 4: the cases of AWS's SigV4 test suite that apply to S3, signed both ways (Authorization
// header and presigned query). Generated from awslabs/aws-c-auth, tests/aws-signing-test-suite/v4 (Apache-2.0), with
// its example keys and the time 2015-08-30T12:36:00Z, service "service", region us-east-1, links valid 3600 s.
// Left out: the path normalisation cases (S3 signs the path as sent; their "-unnormalized" twins are here) and
// "post-sts-header-after" (a session token added after signing; S3 wants every x-amz-* header signed).
#include "s3/SigV4.h"

#include <catch2/catch_test_macros.hpp>

using namespace st::s3;

namespace {

struct SuiteCase {
    const char* name;
    const char* method;
    const char* path;  // as in the request, not encoded
    Params query;      // decoded
    Params headers;    // without Host
    const char* body;
    bool signBody;     // the suite's sign_body: the body's hash is sent as x-amz-content-sha256
    const char* token;
    const char* headerCanonicalRequest;
    const char* headerStringToSign;
    const char* headerSignature;
    const char* queryCanonicalRequest;
    const char* queryStringToSign;
    const char* querySignature;
};

constexpr int64_t kSuiteTime = 1440938160;  // 2015-08-30T12:36:00Z

// clang-format off
const SuiteCase kSuite[] = {
    {"get-header-key-duplicate", "GET", "/",
     {},
     {{"My-Header1", "value2"}, {"My-Header1", "value2"}, {"My-Header1", "value1"}},
     "", false, "",
     "GET\n/\n\nhost:example.amazonaws.com\nmy-header1:value2,value2,value1\nx-amz-date:20150830T123600Z\n\nhost;my-header1;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\ndc7f04a3abfde8d472b0ab1a418b741b7c67174dad1551b4117b15527fbe966c",
     "c9d5ea9f3f72853aea855b47ea873832890dbdd183b4468f858259531a5138ea",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host%3Bmy-header1\nhost:example.amazonaws.com\nmy-header1:value2,value2,value1\n\nhost;my-header1\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nfe8b58fb44117d598520befc07c144a5699c661a8db78f9ce4caee1655dec813",
     "3349ee0b81b4b589da0ff28a395c3591e04de515651dd74f298fa992d1507a97"},
    {"get-header-value-multiline", "GET", "/",
     {},
     {{"My-Header1", "value1\n  value2\n     value3"}},
     "", false, "",
     "GET\n/\n\nhost:example.amazonaws.com\nmy-header1:value1 value2 value3\nx-amz-date:20150830T123600Z\n\nhost;my-header1;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\ne99419459a677bc11de234014be3c4e72c1ea5b454ceb58b613061f5d7a162e8",
     "cfd34249e4b1c8d6b91ef74165d41a32e5fab3306300901bb65a51a73575eefd",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host%3Bmy-header1\nhost:example.amazonaws.com\nmy-header1:value1 value2 value3\n\nhost;my-header1\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n0e73c10e35324b4d215da4bb70be61d13a3d30d569be4ed6e8fd8948965341ca",
     "e6f5def831211aca02987a44b96826706278c7bc078112ae0263659c5b2f2d56"},
    {"get-header-value-order", "GET", "/",
     {},
     {{"My-Header1", "value4"}, {"My-Header1", "value1"}, {"My-Header1", "value3"}, {"My-Header1", "value2"}},
     "", false, "",
     "GET\n/\n\nhost:example.amazonaws.com\nmy-header1:value4,value1,value3,value2\nx-amz-date:20150830T123600Z\n\nhost;my-header1;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n31ce73cd3f3d9f66977ad3dd957dc47af14df92fcd8509f59b349e9137c58b86",
     "08c7e5a9acfcfeb3ab6b2185e75ce8b1deb5e634ec47601a50643f830c755c01",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host%3Bmy-header1\nhost:example.amazonaws.com\nmy-header1:value4,value1,value3,value2\n\nhost;my-header1\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nf4d3f13084ba7664111670ce26458291d3e0c620acd9384f8cd6b60d8e83423e",
     "313720e71ca6202fdcfa9b20f88de01a4eb0638a83c833b1c184359a4eda864e"},
    {"get-header-value-trim", "GET", "/",
     {},
     {{"My-Header1", " value1"}, {"My-Header2", " \"a   b   c\""}},
     "", false, "",
     "GET\n/\n\nhost:example.amazonaws.com\nmy-header1:value1\nmy-header2:\"a b c\"\nx-amz-date:20150830T123600Z\n\nhost;my-header1;my-header2;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\na726db9b0df21c14f559d0a978e563112acb1b9e05476f0a6a1c7d68f28605c7",
     "acc3ed3afb60bb290fc8d2dd0098b9911fcaa05412b367055dee359757a9c736",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host%3Bmy-header1%3Bmy-header2\nhost:example.amazonaws.com\nmy-header1:value1\nmy-header2:\"a b c\"\n\nhost;my-header1;my-header2\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n84c9e353b6161b689210977f93b93e6a7182f9ecb2ceae8af8c3d86b080a88ae",
     "e7bb0fd515e125e1aec2ecc4c0c17484fb06f6846b927c35e46005dd3df3acd4"},
    {"get-relative-relative-unnormalized", "GET", "/example1/example2/../..",
     {},
     {},
     "", false, "",
     "GET\n/example1/example2/../..\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n76115030c0f3ff06c20fdff5ceb6d5e0b835a1743e00b94fea7c7f381269437b",
     "dc33e0856fd4baca4d7aa2146c38958283844764f38c74252a333df5e613003b",
     "GET\n/example1/example2/../..\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n13b7758115ec070fe36a7ae9d754154ad73f6ee9a5eac022494857d0e7effc18",
     "b45db0bfd1cf15003493b733e33aa208dd981bd0e63037a6ed953f71d2118a16"},
    {"get-relative-unnormalized", "GET", "/example/..",
     {},
     {},
     "", false, "",
     "GET\n/example/..\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n0511f456aa502b456d135fcb9d749374a55228f9dbeedda1eacf659e05b0615b",
     "eca7ead57bb5aa5c8e28007acd4ff04e1ff9a0ff3b237ec1554a184887ff9282",
     "GET\n/example/..\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n6e02e2dba21d6f269bae339b86ff27a4b7bbc9d88c482abccbfbf8a49d602482",
     "cbcb213b928a077e43275df47b500f1dfaa864ab3f5a18f6b95f4ff0938167ee"},
    {"get-slash-dot-slash-unnormalized", "GET", "/./",
     {},
     {},
     "", false, "",
     "GET\n/./\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nd67825e2268bd77a97c7688b8d72c31a3c1855b309808505ba0a9747d2465aa7",
     "68714168e6557f8f2de0ef956fc24dc2593a4bd2961f8df51898d8a134695145",
     "GET\n/./\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n21b2da17873f30c1a7410efdc271738b318ccc2c9c6fbe1289fc242aeb1a8ae1",
     "a9b13e8f3484d9505bf1a6f347219f8f35b0fe8f128ceea597efc146a3dfe90c"},
    {"get-slash-pointless-dot-unnormalized", "GET", "/./example",
     {},
     {},
     "", false, "",
     "GET\n/./example\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n73895e0e829507e28e39fd24669aedc2434a8e179e547e3c075b42921f952cdb",
     "beb03f223f7deae4146464f06e29eebbee9c8afbe15c290cf07aa8b119e14cff",
     "GET\n/./example\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\ncb23444939471187ac1f0fb25d437337af0d5f48b5ae0ede5baa3727c8c6af92",
     "72b11f16d9530b18204bfb71f2d6ab085894c0ed8d352730a6e76234c58b5e10"},
    {"get-slash-unnormalized", "GET", "//",
     {},
     {},
     "", false, "",
     "GET\n//\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n80cb39203d058af815de2b79250ff56e1b73eb9b4718c86556cdc6f150c5d209",
     "c88bcd3d312d75078c0cd961d6deae3f4c754924b01669efcfcb439fd5e5b76e",
     "GET\n//\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\ne10a01494cafa1f2207c4de80c69b7a36d6f2d282aa63ee64a5ca50e175cf730",
     "b2a9542809db687769200f56a844a03f2bd0291d6eb90232d5101c6a579446cd"},
    {"get-slashes-unnormalized", "GET", "//example//",
     {},
     {},
     "", false, "",
     "GET\n//example//\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n528ec3105ee1f34ab014bb0a1a45da0ed2742a4fea3555149e5b4d5d201eb240",
     "87cca117541a147f6df867677d98a7d80dff226d2bfca9e4ffa899665623c7e5",
     "GET\n//example//\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n82241114f9a6320f054333c890952474aa7a0069d28a4326c3a8d95bbecc69ee",
     "822a5a9ba28072c5ab2bb4a6307d0c88276d40e49fec6b724c03fb4d4ba60fc2"},
    {"get-space-unnormalized", "GET", "/example space/",
     {},
     {},
     "", false, "",
     "GET\n/example%20space/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n63ee75631ed7234ae61b5f736dfc7754cdccfedbff4b5128a915706ee9390d86",
     "652487583200325589f1fba4c7e578f72c47cb61beeca81406b39ddec1366741",
     "GET\n/example%20space/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\ncc3dd817bd405e51225826ec4934a96d065d6af5b6b0163c7a5abbd26a84519e",
     "7a1f416954786484c9824d93c1f26ef64acb9b1b6c9154d08c9f07d0e394abf6"},
    {"get-unreserved", "GET", "/-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
     {},
     {},
     "", false, "",
     "GET\n/-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n6a968768eefaa713e2a6b16b589a8ea192661f098f37349f4e2c0082757446f9",
     "07ef7494c76fa4850883e2b006601f940f8a34d404d0cfa977f52a65bbf5f24f",
     "GET\n/-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n3134f2fbc6ef58f34b74b01643a159bfccbc121ea4288a4b75e65bc805d08219",
     "95968482db1b9e0fadef6efc1bd24689f77c77d9ef56919c96a28cc92e0d6005"},
    {"get-utf8", "GET", "/\341\210\264",
     {},
     {},
     "", false, "",
     "GET\n/%E1%88%B4\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n2a0a97d02205e45ce2e994789806b19270cfbbb0921b278ccf58f5249ac42102",
     "8318018e0b0f223aa2bbf98705b62bb787dc9c0e678f255a891fd03141be5d85",
     "GET\n/%E1%88%B4\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nd8f78e05d0d9bb1c16ae1e60ed405de76cfb8fdcd3e414ffe65fad563b996d66",
     "10eae3f14a260bd3911cc6d008d3c576d143b05b62f09782a7a4b37f52178e44"},
    {"get-vanilla", "GET", "/",
     {},
     {},
     "", false, "",
     "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nbb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
     "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nbb7705b4aa3cb8e8f5e1e0b3d4c0b64030797a313c8ceee43e33117cc43eadc5",
     "e93c787ed7f371d5c6b165c1b38ede9550f4dce4144713e844b25b7192d3865d"},
    {"get-vanilla-empty-query-key", "GET", "/",
     {{"Param1", "value1"}},
     {},
     "", false, "",
     "GET\n/\nParam1=value1\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n1e24db194ed7d0eec2de28d7369675a243488e08526e8c1c73571282f7c517ab",
     "a67d582fa61cc504c4bae71f336f98b97f1ea3c7a6bfe1b6e45aec72011b9aeb",
     "GET\n/\nParam1=value1&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n74828b608f939673ed5a839190452a943b0178760f258da766209b21f8ca3f86",
     "49096700cbbaa5753443850f40df10f904fc2fdb544dc9512203cc77c471a9de"},
    {"get-vanilla-query", "GET", "/",
     {},
     {},
     "", false, "",
     "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nbb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
     "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nbb7705b4aa3cb8e8f5e1e0b3d4c0b64030797a313c8ceee43e33117cc43eadc5",
     "e93c787ed7f371d5c6b165c1b38ede9550f4dce4144713e844b25b7192d3865d"},
    {"get-vanilla-query-order-encoded", "GET", "/",
     {{"Param-3", "Value3"}, {"Param", "Value2"}, {"\341\210\264", "Value1"}},
     {},
     "", false, "",
     "GET\n/\n%E1%88%B4=Value1&Param=Value2&Param-3=Value3\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n868294f5c38bd141c4972a373a76654f1418a8e4fc18b2e7903ae45e8ae0ec71",
     "371d3713e185cc334048618a97f809c9ffe339c62934c032af5a0e595648fcac",
     "GET\n/\n%E1%88%B4=Value1&Param=Value2&Param-3=Value3&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n9808d6cdc8245c74e705c68350fc6a62b91778cdd613890e5d177998c02c6f53",
     "c5f1848ceec943ac2ca68ee720460c23aaae30a2300586597ada94c4a65e4787"},
    {"get-vanilla-query-order-key-case", "GET", "/",
     {{"Param2", "value2"}, {"Param1", "value1"}},
     {},
     "", false, "",
     "GET\n/\nParam1=value1&Param2=value2\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0",
     "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500",
     "GET\n/\nParam1=value1&Param2=value2&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nb82878ecb2ab7ad194b9fe79b2946c2a36ee1627a219408089b2d774c1a0cedb",
     "86012e2c9ad4d77369f5d81c11f75158aae4f895a085212cc6d3f923d300bed5"},
    {"get-vanilla-query-unreserved", "GET", "/",
     {{"-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", "-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"}},
     {},
     "", false, "",
     "GET\n/\n-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz=-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nc30d4703d9f799439be92736156d47ccfb2d879ddf56f5befa6d1d6aab979177",
     "9c3e54bfcdf0b19771a7f523ee5669cdf59bc7cc0884027167c21bb143a40197",
     "GET\n/\n-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz=-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nc77f27ef2b499fbfee3f0b3c88bba7057b2b31ee9b62047078a5e0d6be91fd58",
     "8e76a88a7433637b12778d5592799b29ad21ecd6cf6325051c21d86f0acda2bf"},
    {"get-vanilla-utf8-query", "GET", "/",
     {{"\341\210\264", "bar"}},
     {},
     "", false, "",
     "GET\n/\n%E1%88%B4=bar\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\neb30c5bed55734080471a834cc727ae56beb50e5f39d1bff6d0d38cb192a7073",
     "2cdec8eed098649ff3a119c94853b13c643bcf08f8b0a1d91e12c9027818dd04",
     "GET\n/\n%E1%88%B4=bar&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nb2e8ae8e48ec880defebc6f6dc1d9fbc9f1856b33146f2943060ca71a50a7a77",
     "0bdd809b1519ac4f0c1dc3540e2cc46bd0c7f778eda408b2ebf3b913d21ff600"},
    {"get-vanilla-with-session-token", "GET", "/",
     {},
     {},
     "", false, "6e86291e8372ff2a2260956d9b8aae1d763fbf315fa00fa31553b73ebf194267",
     "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\nx-amz-security-token:6e86291e8372ff2a2260956d9b8aae1d763fbf315fa00fa31553b73ebf194267\n\nhost;x-amz-date;x-amz-security-token\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n067b36aa60031588cea4a4cde1f21215227a047690c72247f1d70b32fbbfad2b",
     "07ec1639c89043aa0e3e2de82b96708f198cceab042d4a97044c66dd9f74e7f8",
     "GET\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-Security-Token=6e86291e8372ff2a2260956d9b8aae1d763fbf315fa00fa31553b73ebf194267&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\ncb30434103085aa9e73780fc60dc9a9df818fd2a7b1de12a9f6f4d791f898761",
     "7ff2b50b376cb4d151970630573d6291dc128cc5c2a12ffb237f73cc53f67b6c"},
    {"post-header-key-case", "POST", "/",
     {},
     {},
     "", false, "",
     "POST\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n553f88c9e4d10fc9e109e2aeb65f030801b70c2f6468faca261d401ae622fc87",
     "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nd27fc6fe1afc1d88b248c5ae9194ec0943a693dd6d81d8d815c88a369eb0471e",
     "2ce6e6d2e0cf2f9d1b55fafec88cd20574c31dc2e7631979f71ba2310083e95b"},
    {"post-header-key-sort", "POST", "/",
     {},
     {{"My-Header1", "value1"}},
     "", false, "",
     "POST\n/\n\nhost:example.amazonaws.com\nmy-header1:value1\nx-amz-date:20150830T123600Z\n\nhost;my-header1;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n9368318c2967cf6de74404b30c65a91e8f6253e0a8659d6d5319f1a812f87d65",
     "c5410059b04c1ee005303aed430f6e6645f61f4dc9e1461ec8f8916fdf18852c",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host%3Bmy-header1\nhost:example.amazonaws.com\nmy-header1:value1\n\nhost;my-header1\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n1295ed77e4ef8b18b32815e493e0b0f78ee47615c0ecbebfec7c75709eb58c88",
     "c09d07e0d55871f10f2a6d350d994acf6825a3cae70673d7def55616e6119dd7"},
    {"post-header-value-case", "POST", "/",
     {},
     {{"My-Header1", "VALUE1"}},
     "", false, "",
     "POST\n/\n\nhost:example.amazonaws.com\nmy-header1:VALUE1\nx-amz-date:20150830T123600Z\n\nhost;my-header1;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nd51ced243e649e3de6ef63afbbdcbca03131a21a7103a1583706a64618606a93",
     "cdbc9802e29d2942e5e10b5bccfdd67c5f22c7c4e8ae67b53629efa58b974b7d",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host%3Bmy-header1\nhost:example.amazonaws.com\nmy-header1:VALUE1\n\nhost;my-header1\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n9d4d1de88f0149695a91a01275fc72dc00fb50bdc78e424f012ec860d5f8c41d",
     "0698bc9e4f7f9139065ba2909c0e99da257e8e2a42492a097bcd46d792391fed"},
    {"post-sts-header-before", "POST", "/",
     {},
     {},
     "", false, "AQoDYXdzEPT//////////wEXAMPLEtc764bNrC9SAPBSM22wDOk4x4HIZ8j4FZTwdQWLWsKWHGBuFqwAeMicRXmxfpSPfIeoIYRqTflfKD8YUuwthAx7mSEI/qkPpKPi/kMcGdQrmGdeehM4IC1NtBmUpp2wUE8phUZampKsburEDy0KPkyQDYwT7WZ0wq5VSXDvp75YU9HFvlRd8Tx6q6fE8YQcHNVXAkiY9q6d+xo0rKwT38xVqr7ZD0u0iPPkUL64lIZbqBAz+scqKmlzm8FDrypNC9Yjc8fPOLn9FX9KSYvKTr4rvx3iSIlTJabIQwj2ICCR/oLxBA==",
     "POST\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\nx-amz-security-token:AQoDYXdzEPT//////////wEXAMPLEtc764bNrC9SAPBSM22wDOk4x4HIZ8j4FZTwdQWLWsKWHGBuFqwAeMicRXmxfpSPfIeoIYRqTflfKD8YUuwthAx7mSEI/qkPpKPi/kMcGdQrmGdeehM4IC1NtBmUpp2wUE8phUZampKsburEDy0KPkyQDYwT7WZ0wq5VSXDvp75YU9HFvlRd8Tx6q6fE8YQcHNVXAkiY9q6d+xo0rKwT38xVqr7ZD0u0iPPkUL64lIZbqBAz+scqKmlzm8FDrypNC9Yjc8fPOLn9FX9KSYvKTr4rvx3iSIlTJabIQwj2ICCR/oLxBA==\n\nhost;x-amz-date;x-amz-security-token\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nc237e1b440d4c63c32ca95b5b99481081cb7b13c7e40434868e71567c1a882f6",
     "85d96828115b5dc0cfc3bd16ad9e210dd772bbebba041836c64533a82be05ead",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-Security-Token=AQoDYXdzEPT%2F%2F%2F%2F%2F%2F%2F%2F%2F%2FwEXAMPLEtc764bNrC9SAPBSM22wDOk4x4HIZ8j4FZTwdQWLWsKWHGBuFqwAeMicRXmxfpSPfIeoIYRqTflfKD8YUuwthAx7mSEI%2FqkPpKPi%2FkMcGdQrmGdeehM4IC1NtBmUpp2wUE8phUZampKsburEDy0KPkyQDYwT7WZ0wq5VSXDvp75YU9HFvlRd8Tx6q6fE8YQcHNVXAkiY9q6d%2Bxo0rKwT38xVqr7ZD0u0iPPkUL64lIZbqBAz%2BscqKmlzm8FDrypNC9Yjc8fPOLn9FX9KSYvKTr4rvx3iSIlTJabIQwj2ICCR%2FoLxBA%3D%3D&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n37f7ba7a2ec89e8b29a0d637bcc70d8e8d0fdba854ddfc5bc06a12398cfe8605",
     "46c2e3c63c1dbe7d39f8ada16fe7f001c1f56c5791441565323677f96308871c"},
    {"post-vanilla", "POST", "/",
     {},
     {},
     "", false, "",
     "POST\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n553f88c9e4d10fc9e109e2aeb65f030801b70c2f6468faca261d401ae622fc87",
     "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nd27fc6fe1afc1d88b248c5ae9194ec0943a693dd6d81d8d815c88a369eb0471e",
     "2ce6e6d2e0cf2f9d1b55fafec88cd20574c31dc2e7631979f71ba2310083e95b"},
    {"post-vanilla-empty-query-value", "POST", "/",
     {{"Param1", "value1"}},
     {},
     "", false, "",
     "POST\n/\nParam1=value1\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n9d659678c1756bb3113e2ce898845a0a79dbbc57b740555917687f1b3340fbbd",
     "28038455d6de14eafc1f9222cf5aa6f1a96197d7deb8263271d420d138af7f11",
     "POST\n/\nParam1=value1&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n98241d1c6f938789d34b92e16e08b7fc8fdaa774c5437ba30d505eedae43dd2b",
     "a9ef26247f293bffe40eeff86a0480ba8c14d31503b2e0c3399e3a16bf1a3682"},
    {"post-vanilla-query", "POST", "/",
     {{"Param1", "value1"}},
     {},
     "", false, "",
     "POST\n/\nParam1=value1\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n9d659678c1756bb3113e2ce898845a0a79dbbc57b740555917687f1b3340fbbd",
     "28038455d6de14eafc1f9222cf5aa6f1a96197d7deb8263271d420d138af7f11",
     "POST\n/\nParam1=value1&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host\nhost:example.amazonaws.com\n\nhost\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n98241d1c6f938789d34b92e16e08b7fc8fdaa774c5437ba30d505eedae43dd2b",
     "a9ef26247f293bffe40eeff86a0480ba8c14d31503b2e0c3399e3a16bf1a3682"},
    {"post-x-www-form-urlencoded", "POST", "/",
     {},
     {{"Content-Type", "application/x-www-form-urlencoded"}, {"Content-Length", "13"}},
     "Param1=value1", true, "",
     "POST\n/\n\ncontent-length:13\ncontent-type:application/x-www-form-urlencoded\nhost:example.amazonaws.com\nx-amz-content-sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e\nx-amz-date:20150830T123600Z\n\ncontent-length;content-type;host;x-amz-content-sha256;x-amz-date\n9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nb1edd1d03544c25390e32085d55b57acc9a3961bb59415ff86c45c3d89d16cfb",
     "d3875051da38690788ef43de4db0d8f280229d82040bfac253562e56c3f20e0b",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=content-length%3Bcontent-type%3Bhost\ncontent-length:13\ncontent-type:application/x-www-form-urlencoded\nhost:example.amazonaws.com\n\ncontent-length;content-type;host\n9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nee5059a7c437165a28d0e775e6498be428761255d657d8c04cb1baa41de6514c",
     "89a40deed0f26f9461242825a082d2222717248abc7ab41f552ad84a94ad46e9"},
    {"post-x-www-form-urlencoded-parameters", "POST", "/",
     {},
     {{"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}, {"Content-Length", "13"}},
     "Param1=value1", true, "",
     "POST\n/\n\ncontent-length:13\ncontent-type:application/x-www-form-urlencoded; charset=utf-8\nhost:example.amazonaws.com\nx-amz-content-sha256:9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e\nx-amz-date:20150830T123600Z\n\ncontent-length;content-type;host;x-amz-content-sha256;x-amz-date\n9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\na89f1a5b53e37702ee6363ce1da3ce8f54386f3c8f352ae652153c2982a0bc4d",
     "328d1b9eaadca9f5818ef05e8392801e091653bafec24fcab71e7344e7f51422",
     "POST\n/\nX-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIDEXAMPLE%2F20150830%2Fus-east-1%2Fservice%2Faws4_request&X-Amz-Date=20150830T123600Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=content-length%3Bcontent-type%3Bhost\ncontent-length:13\ncontent-type:application/x-www-form-urlencoded; charset=utf-8\nhost:example.amazonaws.com\n\ncontent-length;content-type;host\n9095672bbd1f56dfc5b65f3e153adc8731a4a654192329106275f4c7b24d0b6e",
     "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n32192353b8ae6d857fac1c71cda066e6af2d3a3127ea7ac1fee22621ab4f05dc",
     "0dbeb9b026c7b6675f266b8427efec9b4fa8b1f6ef1477d717aea231106eab4d"},
};
// clang-format on

AwsCredentials suiteKeys(const SuiteCase& c) {
    AwsCredentials k;
    k.accessKeyId = "AKIDEXAMPLE";
    k.secretAccessKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    k.sessionToken = c.token;
    return k;
}

SignInput suiteRequest(const SuiteCase& c) {
    SignInput in;
    in.method = c.method;
    in.host = "example.amazonaws.com";
    in.encodedPath = uriEncode(c.path, false);  // as S3Client encodes keys
    in.query = c.query;
    in.headers = c.headers;
    in.payloadHash = payloadHash(c.body);
    in.region = "us-east-1";
    in.service = "service";
    in.now = kSuiteTime;
    return in;
}

}  // namespace

TEST_CASE("sigv4: AWS's SigV4 test suite, Authorization header (30 cases)") {
    for (const SuiteCase& c : kSuite) {
        CAPTURE(c.name);
        SignInput in = suiteRequest(c);
        if (c.signBody) in.headers.emplace_back("X-Amz-Content-Sha256", in.payloadHash);
        const Signed s = sign(in, suiteKeys(c));
        CHECK(s.canonicalRequest == c.headerCanonicalRequest);
        CHECK(s.stringToSign == c.headerStringToSign);
        CHECK(s.signature == c.headerSignature);
    }
}

TEST_CASE("sigv4: AWS's SigV4 test suite, presigned query (30 cases)") {
    for (const SuiteCase& c : kSuite) {
        CAPTURE(c.name);
        const Signed s = presign(suiteRequest(c), suiteKeys(c), 3600);
        CHECK(s.canonicalRequest == c.queryCanonicalRequest);
        CHECK(s.stringToSign == c.queryStringToSign);
        CHECK(s.signature == c.querySignature);
        // The query sent is the canonical one (the third line of the canonical request) plus the signature.
        const std::string cr = c.queryCanonicalRequest;
        const size_t a = cr.find('\n', cr.find('\n') + 1) + 1;
        CHECK(s.query == cr.substr(a, cr.find('\n', a) - a) + "&X-Amz-Signature=" + c.querySignature);
    }
}
