#include "update/Updater.h"
#include "util/Log.h"
#include "util/StringConv.h"
#include "util/WinError.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Shell32.lib")

namespace magnifier {

namespace {

constexpr const wchar_t* kUserAgent = L"Magnifier-Updater/1.0";
constexpr const wchar_t* kHost      = L"api.github.com";
constexpr INTERNET_PORT  kPort      = INTERNET_DEFAULT_HTTPS_PORT;
constexpr DWORD          kMaxBody   = 8 * 1024 * 1024;   // 8 MiB JSON cap
constexpr DWORD          kTimeoutMs = 15000;

// ----- WinHTTP RAII handles ------------------------------------------------
struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET p) noexcept : h(p) {}
    ~WinHttpHandle() { if (h) ::WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&)            = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) { if (h) ::WinHttpCloseHandle(h); h = o.h; o.h = nullptr; }
        return *this;
    }
    explicit operator bool() const noexcept { return h != nullptr; }
};

struct HttpResponse {
    DWORD       status = 0;
    std::string body;
    std::string location;     // populated for 3xx
    std::string error;        // human-readable on failure
};

// Open a session + connection + request. The caller hands us a host and a
// path; we follow up to `max_redirects` 3xx Location hops (WinHTTP follows
// HTTPS->HTTPS automatically but NOT HTTPS->HTTPS-of-a-different-host without
// us setting WINHTTP_OPTION_REDIRECT_POLICY, and we explicitly need it for
// objects.githubusercontent.com).
HttpResponse HttpGetJson(const std::wstring& host, INTERNET_PORT port,
                         const std::wstring& path,
                         const std::vector<std::wstring>& extra_headers) {
    HttpResponse r;
    WinHttpHandle session(::WinHttpOpen(kUserAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        r.error = "WinHttpOpen failed: " + LastErrorString();
        return r;
    }
    ::WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    // Force TLS 1.2+ — GitHub no longer accepts older.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    ::WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS,
                       &protocols, sizeof(protocols));

    WinHttpHandle conn(::WinHttpConnect(session.h, host.c_str(), port, 0));
    if (!conn) {
        r.error = "WinHttpConnect failed: " + LastErrorString();
        return r;
    }
    WinHttpHandle req(::WinHttpOpenRequest(conn.h, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!req) {
        r.error = "WinHttpOpenRequest failed: " + LastErrorString();
        return r;
    }
    // Allow redirects (default is already follow-all, but be explicit).
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY,
                       &redir, sizeof(redir));

    std::wstring hdrs;
    for (const auto& h : extra_headers) { hdrs += h; hdrs += L"\r\n"; }

    if (!::WinHttpSendRequest(req.h,
            hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
            hdrs.empty() ? 0u : static_cast<DWORD>(hdrs.size()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        r.error = "WinHttpSendRequest failed: " + LastErrorString();
        return r;
    }
    if (!::WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = "WinHttpReceiveResponse failed: " + LastErrorString();
        return r;
    }

    DWORD status = 0, sz = sizeof(status);
    ::WinHttpQueryHeaders(req.h,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = status;

    // Drain body.
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(req.h, &avail)) {
            r.error = "WinHttpQueryDataAvailable failed: " + LastErrorString();
            return r;
        }
        if (avail == 0) break;
        if (r.body.size() + avail > kMaxBody) {
            r.error = "Response too large";
            return r;
        }
        const size_t old = r.body.size();
        r.body.resize(old + avail);
        DWORD read = 0;
        if (!::WinHttpReadData(req.h, r.body.data() + old, avail, &read)) {
            r.error = "WinHttpReadData failed: " + LastErrorString();
            return r;
        }
        r.body.resize(old + read);
    }
    return r;
}

// Split a URL of the form https://host[:port]/path into host/port/path.
// Returns false on parse failure.
bool ParseUrl(std::wstring_view url, std::wstring& host,
              INTERNET_PORT& port, std::wstring& path, bool& secure) {
    constexpr std::wstring_view kHttps = L"https://";
    constexpr std::wstring_view kHttp  = L"http://";
    secure = true;
    std::wstring_view rest;
    if (url.substr(0, kHttps.size()) == kHttps) {
        rest = url.substr(kHttps.size());
        secure = true;
    } else if (url.substr(0, kHttp.size()) == kHttp) {
        rest = url.substr(kHttp.size());
        secure = false;
    } else {
        return false;
    }
    const auto slash = rest.find(L'/');
    std::wstring_view hp = (slash == std::wstring_view::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::wstring_view::npos) ? std::wstring(L"/")
                                              : std::wstring(rest.substr(slash));
    const auto colon = hp.find(L':');
    if (colon == std::wstring_view::npos) {
        host = std::wstring(hp);
        port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    } else {
        host = std::wstring(hp.substr(0, colon));
        try {
            port = static_cast<INTERNET_PORT>(std::stoi(std::wstring(hp.substr(colon + 1))));
        } catch (...) {
            return false;
        }
    }
    return !host.empty();
}

// Streaming download to a file, with optional progress callback. Follows
// redirects automatically (WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS).
bool HttpDownloadToFile(const std::wstring& url,
                        const std::vector<std::wstring>& extra_headers,
                        const std::wstring& dest_path,
                        Updater::ProgressCallback pcb,
                        std::string& error_out) {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = true;
    if (!ParseUrl(url, host, port, path, secure)) {
        error_out = "Invalid URL";
        return false;
    }

    WinHttpHandle session(::WinHttpOpen(kUserAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { error_out = "WinHttpOpen failed: " + LastErrorString(); return false; }
    ::WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, 60000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    ::WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS,
                       &protocols, sizeof(protocols));

    WinHttpHandle conn(::WinHttpConnect(session.h, host.c_str(), port, 0));
    if (!conn) { error_out = "WinHttpConnect failed: " + LastErrorString(); return false; }

    WinHttpHandle req(::WinHttpOpenRequest(conn.h, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) { error_out = "WinHttpOpenRequest failed: " + LastErrorString(); return false; }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    std::wstring hdrs;
    for (const auto& h : extra_headers) { hdrs += h; hdrs += L"\r\n"; }

    if (!::WinHttpSendRequest(req.h,
            hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
            hdrs.empty() ? 0u : static_cast<DWORD>(hdrs.size()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error_out = "WinHttpSendRequest failed: " + LastErrorString();
        return false;
    }
    if (!::WinHttpReceiveResponse(req.h, nullptr)) {
        error_out = "WinHttpReceiveResponse failed: " + LastErrorString();
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    ::WinHttpQueryHeaders(req.h,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        error_out = "HTTP " + std::to_string(status);
        return false;
    }

    int64_t total = 0;
    {
        DWORD len = 0; sz = sizeof(len);
        if (::WinHttpQueryHeaders(req.h,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX)) {
            total = static_cast<int64_t>(len);
        }
    }

    // Write to a .partial sibling then rename atomically.
    const std::wstring partial = dest_path + L".partial";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
        error_out = "Cannot open output file: " + WideToUtf8(partial);
        return false;
    }

    int64_t downloaded = 0;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(req.h, &avail)) {
            error_out = "WinHttpQueryDataAvailable: " + LastErrorString();
            return false;
        }
        if (avail == 0) break;
        while (avail > 0) {
            const DWORD chunk = (std::min)(avail, static_cast<DWORD>(buf.size()));
            DWORD read = 0;
            if (!::WinHttpReadData(req.h, buf.data(), chunk, &read)) {
                error_out = "WinHttpReadData: " + LastErrorString();
                return false;
            }
            if (read == 0) break;
            out.write(buf.data(), read);
            if (!out) { error_out = "Write failed"; return false; }
            downloaded += read;
            avail      -= read;
            if (pcb) pcb(downloaded, total);
        }
    }
    out.close();

    // Atomic-ish rename.
    ::DeleteFileW(dest_path.c_str());
    if (!::MoveFileExW(partial.c_str(), dest_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error_out = "MoveFileEx failed: " + LastErrorString();
        return false;
    }
    return true;
}

// URL-encode the path segments we paste in (owner, repo). Conservative —
// only allow A-Za-z0-9._- through.
std::wstring EscapeSegment(std::string_view in) {
    std::wstring out;
    out.reserve(in.size());
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(hex[c >> 4]));
            out.push_back(static_cast<wchar_t>(hex[c & 0xF]));
        }
    }
    return out;
}

bool EndsWithICase(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(
            s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

} // namespace

// ===========================================================================
struct Updater::Impl {
    mutable std::mutex          mu;
    Settings                    settings;
    std::atomic<bool>           busy{false};
};

Updater::Updater()  : p_(std::make_unique<Impl>()) {}
Updater::~Updater() = default;

void Updater::SetSettings(const Settings& s) {
    std::scoped_lock lk(p_->mu);
    p_->settings = s;
}

Updater::Settings Updater::GetSettings() const {
    std::scoped_lock lk(p_->mu);
    return p_->settings;
}

int Updater::CompareVersions(std::string_view a, std::string_view b) {
    auto strip = [](std::string_view v) {
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
        return v;
    };
    auto next = [](std::string_view& v) -> std::optional<long long> {
        if (v.empty()) return std::nullopt;
        // skip leading dots
        while (!v.empty() && v.front() == '.') v.remove_prefix(1);
        if (v.empty()) return std::nullopt;
        size_t i = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
        if (i == 0) {
            // non-numeric segment: skip up to next dot
            while (i < v.size() && v[i] != '.') ++i;
            v.remove_prefix(i);
            return 0;   // treat as 0 for comparison purposes
        }
        long long n = 0;
        for (size_t k = 0; k < i; ++k) n = n * 10 + (v[k] - '0');
        v.remove_prefix(i);
        return n;
    };
    auto av = strip(a);
    auto bv = strip(b);
    while (!av.empty() || !bv.empty()) {
        auto x = next(av).value_or(0);
        auto y = next(bv).value_or(0);
        if (x < y) return -1;
        if (x > y) return  1;
        if (av.empty() && bv.empty()) break;
    }
    return 0;
}

void Updater::CheckAsync(CheckCallback cb) {
    Settings s = GetSettings();
    if (!s.enabled) {
        if (cb) cb({false, false, s.current_version, "Update checks disabled", std::nullopt});
        return;
    }
    if (s.owner.empty() || s.repo.empty()) {
        if (cb) cb({false, false, s.current_version, "Update owner/repo not configured", std::nullopt});
        return;
    }
    if (p_->busy.exchange(true)) {
        if (cb) cb({false, false, s.current_version, "An update operation is already in progress", std::nullopt});
        return;
    }

    std::thread([this, s = std::move(s), cb = std::move(cb)]() {
        UpdateCheckResult res;
        res.current_version = s.current_version;

        const std::wstring path = L"/repos/" + EscapeSegment(s.owner) +
                                  L"/" + EscapeSegment(s.repo) +
                                  L"/releases/latest";

        std::vector<std::wstring> headers = {
            L"Accept: application/vnd.github+json",
            L"X-GitHub-Api-Version: 2022-11-28",
        };
        if (!s.token.empty()) {
            headers.push_back(L"Authorization: Bearer " + Utf8ToWide(s.token));
        }

        HttpResponse r = HttpGetJson(kHost, kPort, path, headers);
        p_->busy.store(false);

        if (!r.error.empty()) {
            res.error = r.error;
            spdlog::warn("Updater: HTTP error: {}", r.error);
            if (cb) cb(res);
            return;
        }
        if (r.status == 404) {
            res.error = "Release not found (404). For a private repo, set update.token in config.";
            if (cb) cb(res);
            return;
        }
        if (r.status == 401 || r.status == 403) {
            res.error = "Auth failed (" + std::to_string(r.status) +
                        "). Check your update.token (needs Contents: Read on the repo).";
            if (cb) cb(res);
            return;
        }
        if (r.status < 200 || r.status >= 300) {
            res.error = "HTTP " + std::to_string(r.status);
            if (cb) cb(res);
            return;
        }

        try {
            auto j = nlohmann::json::parse(r.body);
            UpdateInfo info;
            info.tag_name     = j.value("tag_name", "");
            info.release_name = j.value("name", "");
            info.release_url  = j.value("html_url", "");
            info.notes        = j.value("body", "");
            info.version      = info.tag_name;
            if (!info.version.empty() && (info.version[0] == 'v' || info.version[0] == 'V')) {
                info.version.erase(0, 1);
            }

            if (j.contains("assets") && j["assets"].is_array()) {
                for (const auto& a : j["assets"]) {
                    const std::string name = a.value("name", "");
                    const std::string url  = a.value("url",  "");      // API URL
                    const int64_t     size = a.value("size", int64_t{0});
                    if (EndsWithICase(name, ".msi")) {
                        info.msi_asset_name = name;
                        info.msi_asset_url  = url;
                        info.msi_asset_size = size;
                    } else if (EndsWithICase(name, ".zip")) {
                        info.zip_asset_name = name;
                        info.zip_asset_url  = url;
                        info.zip_asset_size = size;
                    }
                }
            }

            res.ok = true;
            res.info = std::move(info);
            res.update_available =
                !res.info->version.empty() &&
                CompareVersions(res.info->version, s.current_version) > 0;

            spdlog::info("Updater: current={} latest={} available={}",
                         s.current_version, res.info->version, res.update_available);
        } catch (const std::exception& e) {
            res.error = std::string("JSON parse failed: ") + e.what();
        }

        if (cb) cb(res);
    }).detach();
}

void Updater::DownloadMsiAsync(UpdateInfo info, std::wstring dest_dir,
                               DownloadCallback cb, ProgressCallback pcb) {
    Settings s = GetSettings();
    if (info.msi_asset_url.empty()) {
        if (cb) cb(false, L"", "No .msi asset in latest release");
        return;
    }
    if (p_->busy.exchange(true)) {
        if (cb) cb(false, L"", "An update operation is already in progress");
        return;
    }
    std::thread([this, info = std::move(info), dest_dir = std::move(dest_dir),
                 cb = std::move(cb), pcb = std::move(pcb), s]() {
        const std::wstring url       = Utf8ToWide(info.msi_asset_url);
        const std::wstring filename  = Utf8ToWide(info.msi_asset_name.empty()
                                                    ? "Magnifier.msi"
                                                    : info.msi_asset_name);
        const std::wstring full_path = dest_dir + L"\\" + filename;

        // Authenticated download against api.github.com/repos/.../releases/assets/{id}
        // Required Accept header: application/octet-stream — without it the
        // server returns a 302 redirect to the JSON description instead of
        // the binary.
        std::vector<std::wstring> headers = {
            L"Accept: application/octet-stream",
            L"X-GitHub-Api-Version: 2022-11-28",
        };
        if (!s.token.empty()) {
            headers.push_back(L"Authorization: Bearer " + Utf8ToWide(s.token));
        }

        std::string err;
        const bool ok = HttpDownloadToFile(url, headers, full_path, pcb, err);
        p_->busy.store(false);
        if (!ok) {
            spdlog::warn("Updater: download failed: {}", err);
            if (cb) cb(false, full_path, err);
            return;
        }
        spdlog::info("Updater: downloaded MSI to {}", WideToUtf8(full_path));
        if (cb) cb(true, full_path, "");
    }).detach();
}

bool Updater::LaunchInstaller(const std::wstring& msi_path, bool silent) {
    // msiexec /i <path> [/passive | /qb] /norestart
    const wchar_t* mode = silent ? L"/passive" : L"/qb";
    std::wstring args = L"/i \"";
    args += msi_path;
    args += L"\" ";
    args += mode;
    args += L" /norestart";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd         = nullptr;
    sei.lpVerb       = nullptr;
    sei.lpFile       = L"msiexec.exe";
    sei.lpParameters = args.c_str();
    sei.nShow        = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&sei)) {
        spdlog::error("Updater: ShellExecuteExW(msiexec) failed: {}", LastErrorString());
        return false;
    }
    if (sei.hProcess) ::CloseHandle(sei.hProcess);
    return true;
}

} // namespace magnifier
