// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// macOS WebSocket client: NSURLSessionWebSocketTask (macOS 10.15+). The system
// handles TLS (wss://), proxies and HTTP/1.1 upgrade. Compiled with ARC.

#include "WebSocketClient.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <memory>
#include <mutex>

#if ! __has_feature(objc_arc)
 #error "WebSocketClientApple.mm must be compiled with -fobjc-arc"
#endif

namespace studiorider
{
// Shared between the C++ object and the Objective-C blocks/delegate, so that a
// late callback after close() finds "alive == false" instead of a dead object.
struct AppleWsHub
{
    std::mutex mutex;
    bool alive = true;
    bool closedReported = false;
    WebSocketClient::Callbacks callbacks;

    void report (int code, int http, const std::string& reason)
    {
        std::function<void (int, int, const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (! alive || closedReported)
                return;
            closedReported = true;
            cb = callbacks.onClose;
        }
        if (cb) cb (code, http, reason);
    }
};
}

@interface StudioRiderWsDelegate : NSObject <NSURLSessionWebSocketDelegate>
{
@public
    std::shared_ptr<studiorider::AppleWsHub> hub;
}
@end

@implementation StudioRiderWsDelegate

- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task didOpenWithProtocol:(NSString*)protocol
{
    (void) session; (void) task; (void) protocol;
    auto h = hub;
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock (h->mutex);
        if (! h->alive) return;
        cb = h->callbacks.onOpen;
    }
    if (cb) cb();
}

- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode reason:(NSData*)reason
{
    (void) session; (void) task;
    std::string r;
    if (reason != nil)
        r.assign ((const char*) reason.bytes, (size_t) reason.length);
    hub->report ((int) closeCode, 0, r);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error
{
    (void) session;
    int http = 0;
    if ([task.response isKindOfClass:[NSHTTPURLResponse class]])
    {
        http = (int) ((NSHTTPURLResponse*) task.response).statusCode;
        if (http == 101) http = 0;
    }
    int code = 0;
    if ([task isKindOfClass:[NSURLSessionWebSocketTask class]])
        code = (int) ((NSURLSessionWebSocketTask*) task).closeCode;
    std::string msg = error != nil ? std::string (error.localizedDescription.UTF8String ?: "") : std::string();
    hub->report (code == NSURLSessionWebSocketCloseCodeInvalid ? 0 : code, http, msg);
}

@end

namespace studiorider
{

class AppleWebSocketClient final : public WebSocketClient
{
public:
    ~AppleWebSocketClient() override { close(); }

    void open (const std::string& url, Callbacks cb) override
    {
        close();
        hub = std::make_shared<AppleWsHub>();
        hub->callbacks = std::move (cb);

        @autoreleasepool
        {
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u == nil)
            {
                auto h = hub;
                dispatch_async (dispatch_get_global_queue (QOS_CLASS_UTILITY, 0), ^{ h->report (0, 0, "Unsupported address"); });
                return;
            }

            NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForRequest = 30;          // the studio sends its info every 10 s, even with its DAW stopped
            config.timeoutIntervalForResource = 60 * 60 * 24 * 7;
            config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            config.HTTPAdditionalHeaders = @{ @"User-Agent": @"StudioRider-Receiver" };

            queue = [[NSOperationQueue alloc] init];
            queue.maxConcurrentOperationCount = 1;          // callbacks one at a time
            queue.qualityOfService = NSQualityOfServiceUserInitiated;

            delegate = [[StudioRiderWsDelegate alloc] init];
            delegate->hub = hub;
            session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
            task = [session webSocketTaskWithURL:u];
            task.maximumMessageSize = 16 * 1024 * 1024;
            [task resume];
            receiveNext (task, hub);
        }
    }

    void sendText (const std::string& text) override
    {
        @autoreleasepool
        {
            NSURLSessionWebSocketTask* t = task;
            if (t == nil) return;
            NSURLSessionWebSocketMessage* m = [[NSURLSessionWebSocketMessage alloc]
                                               initWithString:[NSString stringWithUTF8String:text.c_str()]];
            [t sendMessage:m completionHandler:^(NSError*) {}];
        }
    }

    void close() override
    {
        if (hub != nullptr)
        {
            std::lock_guard<std::mutex> lock (hub->mutex);
            hub->alive = false;
        }
        @autoreleasepool
        {
            if (task != nil)
                [task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
            if (session != nil)
                [session invalidateAndCancel]; // releases the delegate
            if (queue != nil)
                [queue waitUntilAllOperationsAreFinished];
        }
        task = nil;
        session = nil;
        delegate = nil;
        queue = nil;
        hub.reset();
    }

    bool supportsTls() const noexcept override { return true; }

    int httpGet (const std::string& url, std::string& body, int timeoutMs) override
    {
        @autoreleasepool
        {
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u == nil)
                return 0;
            NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForRequest = timeoutMs / 1000.0;
            config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            config.HTTPAdditionalHeaders = @{ @"User-Agent": @"StudioRider-Receiver" };
            NSURLSession* s = [NSURLSession sessionWithConfiguration:config];

            auto result = std::make_shared<std::pair<int, std::string>> (0, std::string());
            dispatch_semaphore_t done = dispatch_semaphore_create (0);
            NSURLSessionDataTask* t = [s dataTaskWithURL:u completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                if (error == nil && [response isKindOfClass:[NSHTTPURLResponse class]])
                {
                    result->first = (int) ((NSHTTPURLResponse*) response).statusCode;
                    if (data != nil)
                        result->second.assign ((const char*) data.bytes, (size_t) data.length);
                }
                dispatch_semaphore_signal (done);
            }];
            [t resume];
            const bool finished = dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, (int64_t) (timeoutMs + 1000) * NSEC_PER_MSEC)) == 0;
            [s invalidateAndCancel];
            if (! finished)
                return 0;
            body = result->second;
            return result->first;
        }
    }

private:
    static void receiveNext (NSURLSessionWebSocketTask* t, std::shared_ptr<AppleWsHub> h)
    {
        __weak NSURLSessionWebSocketTask* weakTask = t;
        [t receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* msg, NSError* error) {
            if (error != nil || msg == nil)
                return; // didCompleteWithError / didClose reports it
            {
                std::unique_lock<std::mutex> lock (h->mutex);
                if (! h->alive) return;
                auto onBinary = h->callbacks.onBinary;
                auto onText = h->callbacks.onText;
                lock.unlock();
                if (msg.type == NSURLSessionWebSocketMessageTypeData)
                {
                    if (onBinary) onBinary ((const std::uint8_t*) msg.data.bytes, (std::size_t) msg.data.length);
                }
                else if (onText && msg.string != nil)
                {
                    onText (std::string (msg.string.UTF8String ?: ""));
                }
            }
            NSURLSessionWebSocketTask* strong = weakTask;
            if (strong != nil)
                receiveNext (strong, h);
        }];
    }

    std::shared_ptr<AppleWsHub> hub;
    NSURLSession* session = nil;
    NSURLSessionWebSocketTask* task = nil;
    StudioRiderWsDelegate* delegate = nil;
    NSOperationQueue* queue = nil;
};

std::unique_ptr<WebSocketClient> makeWebSocketClient()
{
    return std::make_unique<AppleWebSocketClient>();
}

} // namespace studiorider
