// Standalone fake S3 service for manual / end-to-end testing of S3 sessions (native/tests/s3/TestS3Server.h).
// Usage: st_tests3server [--port N] [--tls] [--key ID] [--secret S] [--anonymous] [--region R] [--bucket NAME]...
//                        [--demo]
//   --bucket NAME   a bucket (repeatable; default: "demo")
//   --demo          some objects in the first bucket: readme.txt, docs/guide.md, photos/a.jpg, big.bin (40 MB)
// Every request's signature is checked with the key ID and secret (default AKIDTEST / test-secret).
// Prints "LISTENING <endpoint> <certificate SHA-256 or ->" and runs until stdin is closed. Lines on stdin:
//   "keys"   the keys of every bucket       "stats"   bad signatures and open multipart uploads
#include "TestS3Server.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    st::test::TestS3Options o;
    std::vector<std::string> buckets;
    bool demo = false;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(k + " needs a value");
            return argv[++i];
        };
        try {
            if (k == "--port") o.port = std::stoi(value());
            else if (k == "--tls") o.tls = true;
            else if (k == "--key") o.accessKeyId = value();
            else if (k == "--secret") o.secret = value();
            else if (k == "--anonymous") o.allowAnonymous = true;
            else if (k == "--region") o.region = value();
            else if (k == "--bucket") buckets.push_back(value());
            else if (k == "--demo") demo = true;
            else {
                std::cerr << "unknown option " << k << "\n";
                return 2;
            }
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 2;
        }
    }
    if (buckets.empty()) buckets.push_back("demo");
    try {
        st::test::TestS3Server server(o);
        for (const auto& b : buckets) server.createBucket(b);
        if (demo) {
            server.put(buckets[0], "readme.txt", "Hello from the test S3 server.\n", 0, {{"mtime", "1700000000"}});
            server.put(buckets[0], "docs/guide.md", "# Guide\n\nSome text.\n");
            server.put(buckets[0], "photos/a.jpg", std::string(150000, 'j'));
            server.putGenerated(buckets[0], "big.bin", 40ull << 20);
        }
        std::cout << "LISTENING " << server.endpoint() << " "
                  << (o.tls ? server.http().certificateSha256() : std::string("-")) << std::endl;
        for (std::string line; std::getline(std::cin, line);) {
            if (line == "keys") {
                for (const auto& b : buckets)
                    for (const auto& key : server.keys(b)) std::cout << b << "/" << key << "\n";
            } else if (line == "stats") {
                std::cout << "badSignatures " << server.badSignatures() << " openUploads " << server.openUploads() << "\n";
            }
            std::cout << "." << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
