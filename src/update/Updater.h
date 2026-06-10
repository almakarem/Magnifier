#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace magnifier {

// ---------------------------------------------------------------------------
// Information about the latest GitHub release. Populated by Updater::Check.
// ---------------------------------------------------------------------------
struct UpdateInfo {
    std::string version;        // tag_name with optional leading 'v' stripped
    std::string tag_name;       // raw tag (e.g. "v0.1.0")
    std::string release_url;    // human "html_url" — open in browser
    std::string release_name;   // "name" field
    std::string notes;          // markdown body of the release

    // Asset metadata. The URLs are the API resource URLs
    // (https://api.github.com/repos/{owner}/{repo}/releases/assets/{id}) so
    // that the same code path works for both public and private repos when
    // accompanied by Accept: application/octet-stream + a bearer token.
    std::string msi_asset_url;
    std::string msi_asset_name;
    int64_t     msi_asset_size = 0;
    std::string zip_asset_url;
    std::string zip_asset_name;
    int64_t     zip_asset_size = 0;
};

struct UpdateCheckResult {
    bool                       ok               = false;  // request succeeded
    bool                       update_available = false;  // strictly newer
    std::string                current_version;           // what we are running
    std::string                error;                     // empty on ok
    std::optional<UpdateInfo>  info;                      // present when ok
};

// ---------------------------------------------------------------------------
// Updater — checks the GitHub Releases API for a newer build and (optionally)
// downloads + launches the MSI installer. All network work happens on a
// detached worker thread; callbacks fire on that worker thread, so the
// caller is responsible for marshalling back to the UI if needed.
//
// Private repositories: set Settings::token to a fine-grained PAT with
// "Contents: Read" permission on the target repo. Without a token, only
// public repositories will return a 200 from the API.
// ---------------------------------------------------------------------------
class Updater {
public:
    struct Settings {
        bool        enabled          = true;     // check at all
        bool        auto_download    = false;    // download MSI as soon as found
        std::string owner;                       // GitHub user/org
        std::string repo;                        // repository name
        std::string token;                       // PAT for private repos
        std::string current_version;             // populated from kVersionString
    };

    using CheckCallback    = std::function<void(UpdateCheckResult)>;
    using ProgressCallback = std::function<void(int64_t downloaded, int64_t total)>;
    using DownloadCallback = std::function<void(bool ok, std::wstring local_path,
                                                std::string error)>;

    Updater();
    ~Updater();

    Updater(const Updater&)            = delete;
    Updater& operator=(const Updater&) = delete;

    void SetSettings(const Settings& s);
    Settings GetSettings() const;

    // Asynchronously hit the GitHub Releases API. cb is invoked on the
    // worker thread when the request completes (or fails).
    void CheckAsync(CheckCallback cb);

    // Asynchronously download the MSI asset from `info` into `dest_dir`. The
    // resulting path is passed to `cb`. `pcb` is called periodically with
    // (downloaded, total) bytes; total may be 0 if the server didn't send a
    // Content-Length header.
    void DownloadMsiAsync(UpdateInfo info, std::wstring dest_dir,
                          DownloadCallback cb,
                          ProgressCallback pcb = nullptr);

    // Launch msiexec on the downloaded MSI. The current process should exit
    // shortly afterwards so the installer can replace the running binary.
    // When `silent` is true the user sees no UI (passive: small progress bar).
    static bool LaunchInstaller(const std::wstring& msi_path, bool silent);

    // Compare two dotted versions ("1.2.3" or "v1.2.3"). Missing components
    // count as 0. Non-numeric segments compare lexicographically. Returns
    // >0 if a > b, 0 if equal, <0 if a < b.
    static int CompareVersions(std::string_view a, std::string_view b);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace magnifier
