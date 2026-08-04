#pragma once

#include <memory>
#include <string>

namespace Gravitaris {

// Somewhere a line of text reaches a human who isn't watching the server.
// Notify() is called from the sim tick, so an implementation must hand the
// work off rather than do it inline -- a DNS lookup on the tick thread is a
// visible hitch for everyone still flying.
class INotifier {
public:
    virtual ~INotifier() = default;

    virtual void Notify(const std::string& text) = 0;
};

// Posts to a Discord channel webhook. Null when `url` is empty or the build
// left notifications out, so a caller can hold one unconditionally and read
// null as "nobody is listening".
std::unique_ptr<INotifier> MakeWebhookNotifier(std::string url);

// GRAVITARIS_NOTIFY_WEBHOOK, or empty when unset. A webhook URL is a
// credential -- anyone holding it can post to the channel -- so it comes from
// the environment rather than a config file that lives in the repository.
std::string WebhookUrlFromEnvironment();

} // namespace Gravitaris
