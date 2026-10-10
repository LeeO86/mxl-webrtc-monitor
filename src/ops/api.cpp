#include "ops/api.hpp"

#include "tally/tsl.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mwm
{
namespace
{
std::string jsonString(std::string const& text)
{
    return picojson::value(text).serialize();
}

bool mxlRootMounted(std::string const& path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

// <base>/<prefix>/ch<n>/whep and .../index.m3u8; the default bases are the own MediaMTX on MONITOR_PUBLIC_IP.
std::string playbackJson(Config const& cfg, std::string const& path)
{
    auto const suffix = "/" + path;
    bool const whepPublic = !cfg.preview_whep_url.empty();
    bool const hlsPublic = !cfg.preview_hls_url.empty();
    auto const whep = whepPublic ? cfg.preview_whep_url + suffix + "/whep"
                                 : "http://" + cfg.monitor_public_ip + ":" + std::to_string(cfg.mediamtx_whep_port) + suffix + "/whep";
    auto const hls = hlsPublic ? cfg.preview_hls_url + suffix + "/index.m3u8"
                               : "http://" + cfg.monitor_public_ip + ":" + std::to_string(cfg.mediamtx_hls_port) + suffix + "/index.m3u8";
    std::ostringstream out;
    out << "\"playback\":{\"whep\":" << jsonString(whep) << ",\"hls\":" << jsonString(hls) << ",\"public\":{\"whep\":" << (whepPublic ? "true" : "false")
        << ",\"hls\":" << (hlsPublic ? "true" : "false") << "}}";
    return out.str();
}

// Query string to values; '+' and %XX are decoded.
std::map<std::string, std::string> queryValues(std::string const& query)
{
    auto decode = [](std::string const& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '+')
            {
                out += ' ';
            }
            else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                     std::isxdigit(static_cast<unsigned char>(text[i + 2])))
            {
                out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
            {
                out += text[i];
            }
        }
        return out;
    };
    std::map<std::string, std::string> values;
    std::stringstream stream(query);
    std::string item;
    while (std::getline(stream, item, '&'))
    {
        if (item.empty())
        {
            continue;
        }
        auto const eq = item.find('=');
        values[decode(item.substr(0, eq))] = eq == std::string::npos ? std::string{} : decode(item.substr(eq + 1));
    }
    return values;
}

// GET /widgets: the operator-screen widgets of this function and their parameters (JSON schema).
std::string widgetsJson(Config const& cfg)
{
    std::ostringstream out;
    out << R"([{"id":"channel","title":"Monitor channel","params":{"type":"object","properties":{"ch":{"type":"integer","minimum":1,"maximum":)"
        << cfg.monitor_channels
        << R"(,"title":"Channel"},"labels":{"type":"boolean","default":true,"title":"Label and tally lamps"},)"
        << R"("meters":{"type":"boolean","default":true,"title":"Audio meters"}},"required":["ch"]},"min_size":{"w":320,"h":200},"version":)"
        << jsonString(kVersion) << "}]";
    return out.str();
}

// The parameters of /widget/channel: ch 1..MONITOR_CHANNELS, labels and meters booleans, theme. Empty when valid.
std::string channelWidgetError(Config const& cfg, std::map<std::string, std::string> const& params)
{
    auto const ch = params.find("ch");
    if (ch == params.end() || ch->second.empty() || ch->second.size() > 2 || ch->second.find_first_not_of("0123456789") != std::string::npos ||
        std::stoi(ch->second) < 1 || std::stoi(ch->second) > cfg.monitor_channels)
    {
        return "ch must be a channel 1.." + std::to_string(cfg.monitor_channels);
    }
    for (char const* key : {"labels", "meters"})
    {
        auto const it = params.find(key);
        if (it != params.end() && it->second != "true" && it->second != "false" && it->second != "1" && it->second != "0")
        {
            return std::string(key) + " must be true or false";
        }
    }
    auto const theme = params.find("theme");
    if (theme != params.end() && theme->second != "dark" && theme->second != "light" && theme->second != "transparent")
    {
        return "theme must be dark, light or transparent";
    }
    return {};
}
} // namespace

Api::Api(std::shared_ptr<ConfigStore> store, std::shared_ptr<ChannelBook> book)
    : store_(std::move(store))
    , book_(std::move(book))
    , startup_(store_->get())
{
    info_.version = kVersion;
    info_.nmos_cpp = kNmosPin;
    info_.mediamtx = kMediamtxPin;
    info_.mxl_version = kMxlPin;
}

void Api::setInfo(ServiceInfo info)
{
    info_ = std::move(info);
}

void Api::setIndexPage(std::string html)
{
    indexPage_ = std::move(html);
}

void Api::setMetrics(std::function<std::string()> metrics)
{
    metrics_ = std::move(metrics);
}

void Api::setNmosRegistered(std::function<bool()> probe)
{
    nmosRegistered_ = std::move(probe);
}

void Api::setMediamtxReachable(std::function<bool()> probe)
{
    mediamtx_ = std::move(probe);
}

void Api::setMediamtxVersion(std::function<std::string()> probe)
{
    mediamtxVersion_ = std::move(probe);
}

void Api::setNmosSummary(std::function<std::string()> summary)
{
    nmosSummary_ = std::move(summary);
}

void Api::setMediamtxProcess(std::function<ProcessState()> probe)
{
    mediamtxProcess_ = std::move(probe);
}

std::string Api::statusJson() const
{
    auto const cfg = store_->get();
    std::ostringstream out;
    out << "{\"version\":" << jsonString(kVersion) << ",\"channels\":" << cfg.monitor_channels
        << ",\"restart_required\":" << (store_->restartRequired() ? "true" : "false") << ",\"preview\":{\"mode\":"
        << jsonString(startup_.previewShared() ? "shared" : "own") << ",\"publish_url\":" << jsonString(startup_.publishUrl())
        << ",\"path_prefix\":" << jsonString(startup_.preview_path_prefix);
    if (mediamtxProcess_)
    {
        auto const process = mediamtxProcess_();
        out << ",\"mediamtx\":{\"running\":" << (process.running ? "true" : "false") << ",\"restarts\":" << process.restarts << "}";
    }
    out << ",\"streams\":[";
    bool first = true;
    for (auto const& view : book_->snapshot())
    {
        out << (first ? "" : ",") << "{\"channel\":" << view.settings.index << ",\"path\":" << jsonString(view.preview_path)
            << ",\"state\":" << jsonString(view.publish_state) << ",\"error\":" << jsonString(view.publish_error) << "}";
        first = false;
    }
    out << "]}}";
    return out.str();
}

std::string Api::infoJson() const
{
    // The sidecar can run another MediaMTX than the pin of the examples.
    auto const running = mediamtxVersion_ ? mediamtxVersion_() : std::string{};
    std::ostringstream out;
    out << "{\"version\":" << jsonString(info_.version) << ",\"label\":" << jsonString(nodeLabel(store_->get()))
        << ",\"mxl_version\":" << jsonString(info_.mxl_version) << ",\"nmos_cpp\":" << jsonString(info_.nmos_cpp)
        << ",\"gstreamer\":" << jsonString(info_.gstreamer) << ",\"mediamtx\":" << jsonString(running.empty() ? "unknown" : running)
        << ",\"mediamtx_pin\":" << jsonString(info_.mediamtx)
        << ",\"encoder_available\":" << jsonString(info_.encoder_available) << "}";
    return out.str();
}

std::string Api::channelsJson() const
{
    auto const cfg = store_->get();
    std::ostringstream out;
    out << "{\"channels\":[";
    auto const views = book_->snapshot();
    bool first = true;
    for (auto const& view : views)
    {
        if (!first)
        {
            out << ',';
        }
        first = false;
        auto leg = [](LegRoute const& route) {
            std::ostringstream text;
            text << "{\"state\":\"" << stateName(route.state) << "\",\"reason\":" << jsonString(route.reason) << ",\"master_enable\":" << (route.master_enable ? "true" : "false")
                 << ",\"mxl_domain_id\":" << (route.domain_id.empty() ? "null" : jsonString(route.domain_id))
                 << ",\"mxl_flow_id\":" << (route.flow_id.empty() ? "null" : jsonString(route.flow_id))
                 << ",\"sender_id\":" << (route.sender_id.empty() ? "null" : jsonString(route.sender_id)) << "}";
            return text.str();
        };
        out << "{\"index\":" << view.settings.index << ",\"video_label\":" << jsonString(view.settings.video_label)
            << ",\"audio_label\":" << jsonString(view.settings.audio_label) << ",\"video\":" << leg(view.video) << ",\"audio\":" << leg(view.audio)
            << ",\"format\":" << jsonString(view.format) << ",\"encoder\":" << jsonString(view.encoder) << ",\"width\":" << view.width
            << ",\"height\":" << view.height << ",\"audio_channels\":" << view.audio_channels << ",\"source_label\":" << jsonString(view.source_label)
            << ",\"preview_height\":" << view.settings.preview_height << ",\"video_bitrate_kbps\":" << view.settings.video_bitrate_kbps
            << ",\"audio_bitrate_kbps\":" << view.settings.audio_bitrate_kbps << ",\"max_fps\":" << view.settings.max_fps
            << ",\"audio_pair\":" << view.settings.audio_pair << ",\"downmix\":" << jsonString(view.settings.downmix)
            << ",\"overlay\":" << (view.settings.overlay ? "true" : "false") << ",\"overlay_label\":" << (view.settings.overlay_label ? "true" : "false")
            << ",\"overlay_source\":" << (view.settings.overlay_source ? "true" : "false")
            << ",\"overlay_format\":" << (view.settings.overlay_format ? "true" : "false") << ",\"tally_text\":" << (view.settings.tally_text ? "true" : "false")
            << ",\"tally\":" << effectiveTally(view.tsl_lh, view.tsl_rh, view.tsl_text_tally) << ",\"tsl_text\":" << jsonString(view.tsl_text)
            << ",\"tsl_lh\":" << view.tsl_lh << ",\"tsl_rh\":" << view.tsl_rh << ",\"tsl_text_tally\":" << view.tsl_text_tally
            << ",\"viewers\":{\"webrtc\":" << view.viewers_webrtc << ",\"hls\":" << view.viewers_hls << "},\"mediamtx\":{\"ready\":" << (view.mediamtx_ready ? "true" : "false") << ",\"tracks\":[";
        for (std::size_t i = 0; i < view.mediamtx_tracks.size(); ++i)
        {
            out << (i != 0 ? "," : "") << jsonString(view.mediamtx_tracks[i]);
        }
        out << "]},\"preview\":{\"path\":" << jsonString(view.preview_path) << ",\"state\":" << jsonString(view.publish_state)
            << ",\"error\":" << jsonString(view.publish_error) << "}," << playbackJson(cfg, view.preview_path) << ",\"meters\":{\"peak_dbfs\":[";
        for (std::size_t i = 0; i < view.peak_dbfs.size(); ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            out << view.peak_dbfs[i];
        }
        out << "],\"rms_dbfs\":[";
        for (std::size_t i = 0; i < view.rms_dbfs.size(); ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            out << view.rms_dbfs[i];
        }
        out << "]}}";
    }
    out << "]}";
    return out.str();
}

std::string Api::eventsJson() const
{
    return std::string("{\"type\":\"status\",") + channelsJson().substr(1);
}

HttpResponse Api::handle(HttpRequest const& request)
{
    auto const cfg = store_->get();
    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html"))
    {
        HttpResponse response;
        response.contentType = "text/html; charset=utf-8";
        response.body = indexPage_.empty() ? std::string("<!doctype html><title>mxl-webrtc-monitor</title><p>UI was not embedded.</p>") : indexPage_;
        return response;
    }
    if (request.method == "GET" && request.path == "/livez")
    {
        return {200, "application/json", "{\"status\":\"live\"}"};
    }
    if (request.method == "GET" && request.path == "/readyz")
    {
        bool const root = mxlRootMounted(cfg.mxl_domain_scan_path);
        bool const nmos = !cfg.nmos_enable || (nmosRegistered_ && nmosRegistered_());
        bool const mtx = mediamtx_ && mediamtx_();
        HttpResponse response;
        response.status = root && nmos && mtx ? 200 : 503;
        std::ostringstream body;
        body << "{\"mxl_root\":" << (root ? "true" : "false") << ",\"nmos\":" << (nmos ? "true" : "false") << ",\"mediamtx\":" << (mtx ? "true" : "false") << "}";
        response.body = body.str();
        return response;
    }
    if (request.method == "GET" && request.path == "/statusz")
    {
        return {200, "application/json", statusJson()};
    }
    if (request.method == "GET" && request.path == "/widgets")
    {
        return {200, "application/json", widgetsJson(cfg)};
    }
    if (request.method == "GET" && request.path.rfind("/widget/", 0) == 0)
    {
        if (request.path != "/widget/channel")
        {
            return {404, "application/json", "{\"error\":\"widget not found\"}"};
        }
        auto const error = channelWidgetError(cfg, queryValues(request.query));
        if (!error.empty())
        {
            return {400, "application/json", std::string("{\"error\":") + jsonString(error) + "}"};
        }
        // The page reads its parameters and picks the widget from the URL. Only these routes may be
        // framed, by WIDGET_FRAME_ANCESTORS.
        HttpResponse response;
        response.contentType = "text/html; charset=utf-8";
        response.body = indexPage_.empty() ? std::string("<!doctype html><title>mxl-webrtc-monitor</title><p>UI was not embedded.</p>") : indexPage_;
        response.headers.emplace_back("Content-Security-Policy", "frame-ancestors " + cfg.widget_frame_ancestors);
        return response;
    }
    if (request.method == "GET" && request.path == "/metrics")
    {
        HttpResponse response;
        response.contentType = "text/plain; version=0.0.4; charset=utf-8";
        response.body = metrics_ ? metrics_() : std::string();
        return response;
    }
    if (request.method == "GET" && request.path == "/api/v1/info")
    {
        return {200, "application/json", infoJson()};
    }
    if (request.method == "GET" && request.path == "/api/v1/channels")
    {
        return {200, "application/json", channelsJson()};
    }
    if (request.method == "GET" && request.path == "/api/v1/nmos")
    {
        return {200, "application/json", nmosSummary_ ? nmosSummary_() : std::string("{\"enabled\":false}")};
    }
    if (request.method == "GET" && request.path == "/api/v1/config")
    {
        return {200, "application/json", configToJson(cfg, store_->origins(), store_->restartRequired())};
    }
    if (request.method == "GET" && request.path == "/api/v1/config.env")
    {
        HttpResponse response;
        response.contentType = "text/plain; charset=utf-8";
        response.body = configToEnv(cfg);
        return response;
    }
    if (request.method == "GET" && request.path == "/api/v1/config/export")
    {
        return {200, "application/json", exportConfigDocument(cfg)};
    }
    if (request.method == "POST" && request.path == "/api/v1/config/import")
    {
        if (cfg.config_file.empty())
        {
            return {409, "application/json", "{\"error\":\"MONITOR_CONFIG_FILE is not set\"}"};
        }
        try
        {
            auto const settings = settingsFromImport(request.body);
            auto const updated = store_->importDocument(settings);
            book_->reset(updated);
            for (auto const& channel : updated.channels)
            {
                book_->setSettings(channel.index, channel);
            }
        }
        catch (ConfigError const& ex)
        {
            return {400, "application/json", std::string("{\"error\":") + jsonString(ex.what()) + "}"};
        }
        return {200, "application/json", configToJson(store_->get(), store_->origins(), store_->restartRequired())};
    }
    if ((request.method == "PATCH") && request.path.rfind("/api/v1/channels/", 0) == 0)
    {
        auto const suffix = request.path.substr(std::string("/api/v1/channels/").size());
        int index = 0;
        try
        {
            index = std::stoi(suffix);
        }
        catch (...)
        {
            return {404, "application/json", "{\"error\":\"channel not found\"}"};
        }
        if (index < 1 || index > cfg.monitor_channels)
        {
            return {404, "application/json", "{\"error\":\"channel not found\"}"};
        }
        std::string err;
        auto const root = json::parse(request.body, &err);
        if (!err.empty() || !root.is<picojson::object>())
        {
            return {400, "application/json", "{\"error\":\"expected JSON object\"}"};
        }
        std::map<std::string, std::string> patch;
        for (auto const& [key, value] : root.get<picojson::object>())
        {
            std::string mapped = key;
            for (auto& c : mapped)
            {
                c = c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            std::string text;
            if (value.is<std::string>())
            {
                text = value.get<std::string>();
            }
            else if (value.is<bool>())
            {
                text = value.get<bool>() ? "true" : "false";
            }
            else if (value.is<double>())
            {
                text = std::to_string(static_cast<long long>(value.get<double>()));
            }
            else
            {
                return {400, "application/json", "{\"error\":\"unsupported field\"}"};
            }
            patch[channelKey(index, mapped)] = text;
        }
        try
        {
            auto updated = store_->updateFile(patch, nullptr);
            for (auto const& channel : updated.channels)
            {
                book_->setSettings(channel.index, channel);
            }
        }
        catch (ConfigError const& ex)
        {
            return {400, "application/json", std::string("{\"error\":") + jsonString(ex.what()) + "}"};
        }
        return {200, "application/json", channelsJson()};
    }
    if (request.method == "PUT" && request.path == "/api/v1/config")
    {
        if (cfg.config_file.empty())
        {
            return {409, "application/json", "{\"error\":\"MONITOR_CONFIG_FILE is not set\"}"};
        }
        try
        {
            auto const path = std::filesystem::temp_directory_path() / "mxl-webrtc-monitor-config.json";
            {
                std::ofstream out(path, std::ios::trunc);
                out << request.body;
            }
            auto file = loadConfigFile(path.string(), nullptr);
            store_->replaceFile(file);
            auto updated = store_->get();
            book_->reset(updated);
            for (auto const& channel : updated.channels)
            {
                book_->setSettings(channel.index, channel);
            }
        }
        catch (ConfigError const& ex)
        {
            return {400, "application/json", std::string("{\"error\":") + jsonString(ex.what()) + "}"};
        }
        return {200, "application/json", configToJson(store_->get(), store_->origins(), store_->restartRequired())};
    }
    return {404, "application/json", "{\"error\":\"not found\"}"};
}
} // namespace mwm
