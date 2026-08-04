#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef GRAVITARIS_WITH_NOTIFICATIONS
#include <curl/curl.h>
#endif

#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/notify/notifier.hpp>

namespace Gravitaris {

// A reconnect loop on a client would otherwise post as fast as it can retry,
// and Discord answers that by disabling the webhook.
static constexpr std::chrono::seconds MIN_POST_INTERVAL{5};

// Past this the backlog is dropped rather than grown: a notification nobody
// read for a minute isn't worth delivering late.
static constexpr std::size_t MAX_QUEUED = 16;

static constexpr long POST_TIMEOUT_SECONDS = 10;

std::string WebhookUrlFromEnvironment()
{
    const char* url = std::getenv("GRAVITARIS_NOTIFY_WEBHOOK");
    return url ? std::string(url) : std::string();
}

#ifdef GRAVITARIS_WITH_NOTIFICATIONS

static std::string JsonEscape(const std::string& text);

namespace {

// One worker thread draining a queue, so Notify() only ever takes a lock and
// signals -- the POST itself, DNS included, never touches the caller.
class WebhookNotifier : public INotifier {
public:
    explicit WebhookNotifier(std::string url)
            : m_url(std::move(url))
    {
        m_worker = std::thread([this] { Run(); });
    }

    ~WebhookNotifier() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_worker.join();
    }

    void Notify(const std::string& text) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.size() >= MAX_QUEUED) return;
            m_queue.push_back(text);
        }
        m_wake.notify_one();
    }

private:
    void Run()
    {
        // Per-thread and per-notifier: curl_easy_init on a handle this thread
        // owns outright is the only threading contract libcurl makes cheaply.
        CURL* curl = curl_easy_init();
        if (!curl) {
            LOG(error) << "notify: curl_easy_init failed, webhook disabled";
            return;
        }

        curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) break;

            std::string text = std::move(m_queue.front());
            m_queue.pop_front();

            lock.unlock();
            Post(curl, headers, text);
            lock.lock();

            // Paced, but interruptible: a shutdown mid-cooldown shouldn't hold
            // the process open for the rest of it.
            m_wake.wait_for(lock, MIN_POST_INTERVAL, [this] { return m_stopping; });
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    void Post(CURL* curl, curl_slist* headers, const std::string& text)
    {
        const std::string body = "{\"content\":\"" + JsonEscape(text) + "\"}";

        curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, POST_TIMEOUT_SECONDS);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Discard);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gravitaris");

        const CURLcode result = curl_easy_perform(curl);
        if (result != CURLE_OK) {
            LOG(warning) << "notify: post failed: " << curl_easy_strerror(result);
            return;
        }

        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        // 204 is Discord's success for a webhook post; anything else is worth
        // seeing, since a 401 here means the URL was revoked.
        if (status < 200 || status >= 300) {
            LOG(warning) << "notify: webhook answered HTTP " << status;
        }
    }

    static std::size_t Discard(char*, std::size_t size, std::size_t count, void*)
    {
        return size * count;
    }

    std::string m_url;
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_queue;
    bool m_stopping = false;
};

} // namespace

std::unique_ptr<INotifier> MakeWebhookNotifier(std::string url)
{
    if (url.empty()) return nullptr;

    LOG(info) << "notify: webhook enabled";
    return std::make_unique<WebhookNotifier>(std::move(url));
}

// Only what RFC 8259 requires, since the payload is a chat line rather than
// arbitrary binary: quotes, backslashes and the control range.
static std::string JsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);

    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escape[7];
                std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                out += escape;
            }
            else {
                out += c;
            }
        }
    }
    return out;
}

#else

std::unique_ptr<INotifier> MakeWebhookNotifier(std::string)
{
    return nullptr;
}

#endif // GRAVITARIS_WITH_NOTIFICATIONS

} // namespace Gravitaris
