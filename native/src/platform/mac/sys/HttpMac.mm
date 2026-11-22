// platform/sys/Http.h on macOS: NSURLSession with a delegate, waited on by the calling thread. The server-trust
// challenge comes after the TLS handshake and before anything is sent: a certificate the system does not trust is
// accepted there only when it is pinned. Certificate details are read with OpenSSL (already linked by SecretMac.cpp).
#include "platform/sys/Http.h"

#import <Foundation/Foundation.h>
#include <CFNetwork/CFNetwork.h>
#include <Security/Security.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>

namespace st::sys {
namespace {

std::string hexOf(const unsigned char* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += kHex[p[i] >> 4];
        out += kHex[p[i] & 15];
    }
    return out;
}

std::string sha256Hex(const std::string& der) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(der.data(), der.size(), md, &len, EVP_sha256(), nullptr);
    return hexOf(md, len);
}

std::string nameCn(X509_NAME* name) {
    if (!name) return {};
    char buf[512] = {};
    if (X509_NAME_get_text_by_NID(name, NID_commonName, buf, sizeof buf) > 0) return buf;
    char* line = X509_NAME_oneline(name, buf, sizeof buf);
    return line ? line : std::string();
}

std::string asn1Date(const ASN1_TIME* t) {
    struct tm tm {};
    if (!t || ASN1_TIME_to_tm(t, &tm) != 1) return {};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string leafDer(SecTrustRef trust) {
    CFArrayRef chain = SecTrustCopyCertificateChain(trust);
    if (!chain) return {};
    std::string der;
    if (CFArrayGetCount(chain) > 0) {
        auto cert = (SecCertificateRef)CFArrayGetValueAtIndex(chain, 0);
        if (CFDataRef data = SecCertificateCopyData(cert)) {
            der.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<size_t>(CFDataGetLength(data)));
            CFRelease(data);
        }
    }
    CFRelease(chain);
    return der;
}

void describe(const std::string& der, bool trusted, const std::string& problem, HttpCertificate& out) {
    out.ok = true;
    out.der = der;
    out.sha256 = sha256Hex(der);
    out.trusted = trusted;
    out.problem = problem;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
    if (X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()))) {
        out.subject = nameCn(X509_get_subject_name(x));
        out.issuer = nameCn(X509_get_issuer_name(x));
        out.validFrom = asn1Date(X509_get0_notBefore(x));
        out.validTo = asn1Date(X509_get0_notAfter(x));
        X509_free(x);
    }
}

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::string str(NSString* s) { return s ? std::string([s UTF8String]) : std::string(); }

/** One request in flight; shared between the waiting thread and the delegate queue. */
struct Pending {
    const HttpRequest* req = nullptr;
    bool probe = false;
    std::vector<std::string> pins;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool stoppedByBody = false;
    bool certRejected = false;
    std::string certProblem;
    HttpCertificate cert;
    HttpResponse resp;
    NSInteger errorCode = 0;
    std::string errorText;
};

}  // namespace
}  // namespace st::sys

@interface STHttpDelegate : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) NSString* proxyUser;
@property(nonatomic) NSString* proxyPassword;
- (void)add:(NSURLSessionTask*)task pending:(std::shared_ptr<st::sys::Pending>)p;
- (void)remove:(NSURLSessionTask*)task;
@end

@implementation STHttpDelegate {
    std::mutex _m;
    std::map<NSUInteger, std::shared_ptr<st::sys::Pending>> _tasks;
}

- (void)add:(NSURLSessionTask*)task pending:(std::shared_ptr<st::sys::Pending>)p {
    std::lock_guard lk(_m);
    _tasks[task.taskIdentifier] = std::move(p);
}

- (void)remove:(NSURLSessionTask*)task {
    std::lock_guard lk(_m);
    _tasks.erase(task.taskIdentifier);
}

- (std::shared_ptr<st::sys::Pending>)find:(NSURLSessionTask*)task {
    std::lock_guard lk(_m);
    auto it = _tasks.find(task.taskIdentifier);
    return it == _tasks.end() ? nullptr : it->second;
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
     didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
       completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))completionHandler {
    auto p = [self find:task];
    NSURLProtectionSpace* space = challenge.protectionSpace;
    if ([space.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust]) {
        SecTrustRef trust = space.serverTrust;
        CFErrorRef err = nullptr;
        const bool trusted = SecTrustEvaluateWithError(trust, &err);
        std::string problem;
        if (err) {
            if (CFStringRef d = CFErrorCopyDescription(err)) {
                problem = st::sys::str((__bridge NSString*)d);
                CFRelease(d);
            }
            CFRelease(err);
        }
        const std::string der = st::sys::leafDer(trust);
        if (p && p->probe) {
            st::sys::describe(der, trusted, problem, p->cert);
            completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);  // nothing is sent
            return;
        }
        bool ok = trusted;
        if (!ok && p) {
            const std::string sha = st::sys::sha256Hex(der);
            ok = std::find(p->pins.begin(), p->pins.end(), sha) != p->pins.end();
        }
        if (ok) {
            completionHandler(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:trust]);
        } else {
            if (p) {
                p->certRejected = true;
                p->certProblem = problem.empty() ? "it is not trusted by this Mac" : problem;
            }
            completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
        }
        return;
    }
    if (space.isProxy && challenge.previousFailureCount < 2) {
        if (self.proxyUser.length) {
            completionHandler(NSURLSessionAuthChallengeUseCredential,
                              [NSURLCredential credentialWithUser:self.proxyUser
                                                         password:self.proxyPassword ?: @""
                                                      persistence:NSURLCredentialPersistenceForSession]);
        } else {
            completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
        }
        return;
    }
    completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    completionHandler(nil);  // never follow: the caller sees the 3xx
}

- (void)URLSession:(NSURLSession*)session
                dataTask:(NSURLSessionDataTask*)task
      didReceiveResponse:(NSURLResponse*)response
       completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    if (auto p = [self find:task]) {
        if (auto http = (NSHTTPURLResponse*)response; [http isKindOfClass:[NSHTTPURLResponse class]]) {
            p->resp.status = static_cast<int>(http.statusCode);
            [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
                p->resp.headers.emplace_back(st::sys::str([key description]), st::sys::str([value description]));
            }];
        }
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    auto p = [self find:task];
    if (!p) return;
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
        const std::string_view chunk(static_cast<const char*>(bytes), range.length);
        if (p->req->onBody && p->resp.status >= 200 && p->resp.status < 300) {
            if (!p->req->onBody(chunk)) {
                p->stoppedByBody = true;
                *stop = YES;
                [task cancel];
            }
        } else {
            p->resp.body.append(chunk);
        }
    }];
}

- (void)URLSession:(NSURLSession*)session
                        task:(NSURLSessionTask*)task
             didSendBodyData:(int64_t)bytesSent
              totalBytesSent:(int64_t)totalBytesSent
    totalBytesExpectedToSend:(int64_t)totalBytesExpectedToSend {
    auto p = [self find:task];
    if (p && p->req->onSent) p->req->onSent(static_cast<uint64_t>(totalBytesSent));
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    auto p = [self find:task];
    if (!p) return;
    {
        std::lock_guard lk(p->m);
        if (error) {
            p->errorCode = error.code;
            p->errorText = st::sys::str(error.localizedDescription);
        }
        p->done = true;
    }
    p->cv.notify_all();
}
@end

namespace st::sys {
namespace {

class MacHttpSession final : public HttpSession {
public:
    explicit MacHttpSession(const HttpSessionOptions& o) : opts_(o), pins_(o.pinnedSha256) {
        @autoreleasepool {
            NSURLSessionConfiguration* c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            c.HTTPCookieStorage = nil;
            c.HTTPShouldSetCookies = NO;
            c.URLCache = nil;
            c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            c.timeoutIntervalForRequest = o.ioTimeoutMs / 1000.0;
            c.HTTPMaximumConnectionsPerHost = 32;
            c.HTTPAdditionalHeaders = @{@"User-Agent" : ns(o.userAgent)};
            if (o.proxy.mode == HttpProxy::Mode::None) {
                c.connectionProxyDictionary = @{};
            } else if (o.proxy.mode == HttpProxy::Mode::Http) {
                NSString* host = ns(o.proxy.host);
                NSNumber* port = @(o.proxy.port);
                c.connectionProxyDictionary = @{
                    (__bridge NSString*)kCFNetworkProxiesHTTPEnable : @1,
                    (__bridge NSString*)kCFNetworkProxiesHTTPProxy : host,
                    (__bridge NSString*)kCFNetworkProxiesHTTPPort : port,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSEnable : @1,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSProxy : host,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSPort : port,
                };
            }
            delegate_ = [STHttpDelegate new];
            delegate_.proxyUser = ns(o.proxy.username);
            delegate_.proxyPassword = ns(o.proxy.password);
            NSOperationQueue* q = [NSOperationQueue new];
            q.maxConcurrentOperationCount = 8;
            session_ = [NSURLSession sessionWithConfiguration:c delegate:delegate_ delegateQueue:q];
        }
    }

    ~MacHttpSession() override { close(); }

    HttpResponse send(const HttpRequest& req) override { return run(req, false, nullptr); }

    HttpCertificate probeCertificate(const std::string& url) override {
        HttpCertificate cert;
        HttpRequest req;
        req.method = "HEAD";
        req.url = url;
        const HttpResponse r = run(req, true, &cert);
        if (!cert.ok) cert.error = r.error.empty() ? "No TLS certificate (not an https:// address?)" : r.error;
        return cert;
    }

    void setPinnedCertificates(std::vector<std::string> sha256) override {
        std::lock_guard lk(m_);
        pins_ = std::move(sha256);
    }

    void close() override {
        NSURLSession* s = nil;
        {
            std::lock_guard lk(m_);
            if (closed_) return;
            closed_ = true;
            s = session_;
        }
        [s invalidateAndCancel];
    }

private:
    HttpResponse run(const HttpRequest& req, bool probe, HttpCertificate* certOut) {
        HttpResponse r;
        UrlParts u;
        if (!splitUrl(req.url, u)) {
            r.error = "Invalid URL: " + req.url;
            return r;
        }
        auto p = std::make_shared<Pending>();
        p->req = &req;
        p->probe = probe;
        NSURLSessionTask* task = nil;
        {
            std::lock_guard lk(m_);
            if (closed_) {
                r.cancelled = true;
                r.error = "The HTTP client is closed";
                return r;
            }
            p->pins = pins_;
            @autoreleasepool {
                NSURL* url = [NSURL URLWithString:ns(req.url)];
                if (!url) {
                    r.error = "Invalid URL: " + req.url;
                    return r;
                }
                NSMutableURLRequest* m = [NSMutableURLRequest requestWithURL:url];
                m.HTTPMethod = ns(req.method);
                m.timeoutInterval = opts_.ioTimeoutMs / 1000.0;
                [m setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];  // bytes as stored, never unpacked
                for (const auto& [n, v] : req.headers) {
                    if (strcasecmp(n.c_str(), "host") == 0) continue;
                    [m setValue:ns(v) forHTTPHeaderField:ns(n)];
                }
                if (req.body) {
                    NSData* body = [NSData dataWithBytes:req.body->data() length:req.body->size()];
                    task = [session_ uploadTaskWithRequest:m fromData:body];
                } else {
                    task = [session_ dataTaskWithRequest:m];
                }
                [delegate_ add:task pending:p];
                [task resume];
            }
        }
        {
            std::unique_lock lk(p->m);
            while (!p->done) {
                p->cv.wait_for(lk, std::chrono::milliseconds(50));
                if (req.cancel && req.cancel->load() && !p->done) [task cancel];
            }
        }
        [delegate_ remove:task];
        if (certOut) *certOut = p->cert;

        HttpResponse out = std::move(p->resp);
        if (p->certRejected) {
            HttpResponse e;
            e.certificateError = true;
            e.error = "The server's certificate is not trusted: " + p->certProblem;
            return e;
        }
        if (p->errorCode != 0 || p->stoppedByBody) {
            HttpResponse e;
            e.cancelled = p->stoppedByBody || p->errorCode == NSURLErrorCancelled;
            e.timedOut = p->errorCode == NSURLErrorTimedOut;
            e.certificateError = p->errorCode == NSURLErrorServerCertificateUntrusted ||
                                 p->errorCode == NSURLErrorServerCertificateHasUnknownRoot ||
                                 p->errorCode == NSURLErrorServerCertificateHasBadDate ||
                                 p->errorCode == NSURLErrorServerCertificateNotYetValid;
            e.error = e.cancelled ? "Cancelled" : p->errorText;
            return e;
        }
        if (req.method != "HEAD" && !(req.onBody && out.status >= 200 && out.status < 300)) {
            const std::string cl = out.header("Content-Length");
            if (!cl.empty() && std::to_string(out.body.size()) != cl) {
                HttpResponse e;
                e.error = "The connection was closed before the whole answer arrived";
                return e;
            }
        }
        return out;
    }

    HttpSessionOptions opts_;
    std::mutex m_;
    std::vector<std::string> pins_;
    bool closed_ = false;
    STHttpDelegate* delegate_ = nil;
    NSURLSession* session_ = nil;
};

}  // namespace

std::shared_ptr<HttpSession> openHttpSession(const HttpSessionOptions& options) {
    return std::make_shared<MacHttpSession>(options);
}

}  // namespace st::sys
