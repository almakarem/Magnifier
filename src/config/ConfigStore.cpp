#include "config/ConfigStore.h"
#include "util/Log.h"
#include "util/StringConv.h"

#include <Windows.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace magnifier {

// ---------------------------------------------------------------------------
// Key-name vocabulary
// ---------------------------------------------------------------------------
namespace {

struct KeyEntry { std::string_view name; unsigned vk; };

constexpr KeyEntry kKeyTable[] = {
    // letters
    {"a", 'A'}, {"b", 'B'}, {"c", 'C'}, {"d", 'D'}, {"e", 'E'}, {"f", 'F'},
    {"g", 'G'}, {"h", 'H'}, {"i", 'I'}, {"j", 'J'}, {"k", 'K'}, {"l", 'L'},
    {"m", 'M'}, {"n", 'N'}, {"o", 'O'}, {"p", 'P'}, {"q", 'Q'}, {"r", 'R'},
    {"s", 'S'}, {"t", 'T'}, {"u", 'U'}, {"v", 'V'}, {"w", 'W'}, {"x", 'X'},
    {"y", 'Y'}, {"z", 'Z'},
    // digits
    {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
    {"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},
    // function keys
    {"f1",  VK_F1},  {"f2",  VK_F2},  {"f3",  VK_F3},  {"f4",  VK_F4},
    {"f5",  VK_F5},  {"f6",  VK_F6},  {"f7",  VK_F7},  {"f8",  VK_F8},
    {"f9",  VK_F9},  {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
    {"f13", VK_F13}, {"f14", VK_F14}, {"f15", VK_F15}, {"f16", VK_F16},
    {"f17", VK_F17}, {"f18", VK_F18}, {"f19", VK_F19}, {"f20", VK_F20},
    {"f21", VK_F21}, {"f22", VK_F22}, {"f23", VK_F23}, {"f24", VK_F24},
    // named
    {"space",      VK_SPACE},
    {"escape",     VK_ESCAPE},
    {"esc",        VK_ESCAPE},
    {"enter",      VK_RETURN},
    {"return",     VK_RETURN},
    {"tab",        VK_TAB},
    {"backspace",  VK_BACK},
    {"insert",     VK_INSERT},
    {"delete",     VK_DELETE},
    {"home",       VK_HOME},
    {"end",        VK_END},
    {"pageup",     VK_PRIOR},
    {"pagedown",   VK_NEXT},
    {"left",       VK_LEFT},
    {"right",      VK_RIGHT},
    {"up",         VK_UP},
    {"down",       VK_DOWN},
    {"plus",       VK_OEM_PLUS},
    {"=",          VK_OEM_PLUS},
    {"minus",      VK_OEM_MINUS},
    {"-",          VK_OEM_MINUS},
    {"[",          VK_OEM_4},
    {"]",          VK_OEM_6},
    {",",          VK_OEM_COMMA},
    {".",          VK_OEM_PERIOD},
    {";",          VK_OEM_1},
    {"'",          VK_OEM_7},
    {"`",          VK_OEM_3},
    {"\\",         VK_OEM_5},
    {"/",          VK_OEM_2},
    {"printscreen",VK_SNAPSHOT},
    {"scrolllock", VK_SCROLL},
    {"pause",      VK_PAUSE},
};

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> Split(std::string_view s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            if (i > start) {
                std::string tok(s.substr(start, i - start));
                // trim
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())))  tok.pop_back();
                if (!tok.empty()) out.push_back(std::move(tok));
            }
            start = i + 1;
        }
    }
    return out;
}

unsigned ModifierFromName(std::string_view n) {
    if (n == "ctrl"  || n == "control") return MOD_CONTROL;
    if (n == "alt")                     return MOD_ALT;
    if (n == "shift")                   return MOD_SHIFT;
    if (n == "win"   || n == "windows" || n == "super") return MOD_WIN;
    return 0;
}

std::optional<unsigned> VkFromName(std::string_view n) {
    for (const auto& e : kKeyTable) {
        if (e.name == n) return e.vk;
    }
    return std::nullopt;
}

const char* NameFromVk(unsigned vk) {
    for (const auto& e : kKeyTable) {
        if (e.vk == vk) return e.name.data();
    }
    return "?";
}

} // namespace

std::optional<HotkeyBinding> ParseHotkey(std::string_view spec) {
    HotkeyBinding b{};
    auto parts = Split(ToLowerAscii(spec), '+');
    if (parts.empty()) return std::nullopt;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const unsigned m = ModifierFromName(parts[i]);
        if (m == 0) return std::nullopt;   // unknown modifier
        b.modifiers |= m;
    }
    auto vk = VkFromName(parts.back());
    if (!vk) return std::nullopt;
    b.vk = *vk;
    return b;
}

std::string HotkeyBinding::to_human() const {
    if (!is_bound()) return "(unbound)";
    std::string s;
    if (modifiers & MOD_CONTROL) s += "Ctrl+";
    if (modifiers & MOD_ALT)     s += "Alt+";
    if (modifiers & MOD_SHIFT)   s += "Shift+";
    if (modifiers & MOD_WIN)     s += "Win+";
    s += NameFromVk(vk);
    return s;
}

// ---------------------------------------------------------------------------
// Default config text (used when the file is missing). Kept in sync with
// packaging/portable/config.default.toml but embedded so the running
// process is self-contained.
// ---------------------------------------------------------------------------
static constexpr std::string_view kEmbeddedDefaultToml = R"TOML(
schema_version = 1

[general]
start_minimized   = false
restore_last_mode = true
active_priority   = "above_normal"

[lens]
width            = 640
height           = 360
border_thickness = 2
border_color     = "#00E5FF"
follow_mouse     = true
# When false the real OS hardware cursor is shown over the lens (smooth, no
# ghosting). Set true to draw a magnified cursor inside the content instead
# (matches system Magnifier; cursor will appear to lag on fast moves).
magnify_cursor   = false
position_tau     = 0.05
zoom_tau         = 0.10

[zoom]
initial      = 2.0
min          = 1.0
max          = 16.0
default_step = 0.25

[capture]
mode = "auto"

[ipc]
pipe_name = "MagnifierCtl"
http_port = 0

[advanced]
low_level_keyboard_hook = false
log_level               = "info"

[update]
# Check the GitHub Releases API on startup for a newer build. If found, a
# tray balloon notification is shown and the Updates tab in Settings lights
# up. The actual install is opt-in (one click).
check_on_startup = true
# If true, the MSI is downloaded automatically in the background after a
# successful check (still requires user confirmation to launch the installer).
auto_download    = false
# GitHub repo to query (owner/repo). Leave owner empty to disable.
owner            = "almakarem"
repo             = "Magnifier"
# Personal Access Token (fine-grained, repo "Contents: Read" only) required
# for *private* repositories. Leave empty for public repos.
token            = ""

[hotkeys]
toggle_lens       = "ctrl+alt+z"
toggle_fullscreen = "ctrl+alt+f"
turn_off          = "ctrl+alt+x"
zoom_in           = "ctrl+alt+plus"
zoom_out          = "ctrl+alt+minus"
lens_size_up      = "ctrl+alt+]"
lens_size_down    = "ctrl+alt+["
recenter          = "ctrl+alt+c"

[controller]
enabled                = true
deadzone               = 0.15
curve                  = 2.0
move_speed             = 800.0
zoom_speed             = 2.0
idle_recenter_seconds  = 1.5

[controller.bindings]
move_x            = "ls_x"
move_y            = "ls_y"
zoom_axis         = "rs_y"
toggle_lens       = "x"
toggle_fullscreen = "y"
turn_off          = "back"
recenter          = "ls"
)TOML";

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
namespace {

template <typename T>
T GetOr(const toml::table& t, std::string_view key, T def) {
    if (auto v = t[key].value<T>()) return *v;
    return def;
}

CaptureMode ParseCaptureMode(std::string_view s) {
    const auto lower = ToLowerAscii(s);
    if (lower == "exclude_self") return CaptureMode::ExcludeSelf;
    if (lower == "include_self") return CaptureMode::IncludeSelf;
    return CaptureMode::Auto;
}

void ParseHotkeyTable(const toml::table& tbl, Config& cfg,
                      std::vector<std::string>& warnings) {
    constexpr std::pair<std::string_view, Action> kMap[] = {
        {"toggle_lens",        Action::ToggleLens},
        {"toggle_fullscreen",  Action::ToggleFullscreen},
        {"turn_off",           Action::TurnOff},
        {"zoom_in",            Action::ZoomIn},
        {"zoom_out",           Action::ZoomOut},
        {"zoom_reset",         Action::ZoomReset},
        {"lens_size_up",       Action::LensSizeUp},
        {"lens_size_down",     Action::LensSizeDown},
        {"recenter",           Action::Recenter},
        {"next_monitor",       Action::NextMonitor},
        {"reload_config",      Action::ReloadConfig},
        {"show_settings",      Action::ShowSettings},
        {"quit",               Action::Quit},
    };
    for (const auto& [name, act] : kMap) {
        if (auto v = tbl[name].value<std::string>()) {
            if (v->empty()) continue;
            if (auto bind = ParseHotkey(*v)) {
                cfg.hotkeys[act] = *bind;
            } else {
                warnings.push_back("Unknown hotkey spec for '"
                                   + std::string(name) + "': " + *v);
            }
        }
    }
}

void ParseController(const toml::table& tbl, Config& cfg) {
    cfg.controller.enabled               = GetOr<bool>  (tbl, "enabled",  true);
    cfg.controller.deadzone              = GetOr<double>(tbl, "deadzone", 0.15);
    cfg.controller.curve                 = GetOr<double>(tbl, "curve",    2.0);
    cfg.controller.move_speed            = GetOr<double>(tbl, "move_speed", 800.0);
    cfg.controller.zoom_speed            = GetOr<double>(tbl, "zoom_speed", 2.0);
    cfg.controller.idle_recenter_seconds = GetOr<double>(tbl, "idle_recenter_seconds", 1.5);
    if (auto b = tbl["bindings"].as_table()) {
        auto& bb = cfg.controller.bindings;
        bb.move_x            = GetOr<std::string>(*b, "move_x",            bb.move_x);
        bb.move_y            = GetOr<std::string>(*b, "move_y",            bb.move_y);
        bb.zoom_axis         = GetOr<std::string>(*b, "zoom_axis",         bb.zoom_axis);
        bb.toggle_lens       = GetOr<std::string>(*b, "toggle_lens",       bb.toggle_lens);
        bb.toggle_fullscreen = GetOr<std::string>(*b, "toggle_fullscreen", bb.toggle_fullscreen);
        bb.turn_off          = GetOr<std::string>(*b, "turn_off",          bb.turn_off);
        bb.recenter          = GetOr<std::string>(*b, "recenter",          bb.recenter);
    }
}

Config ParseToml(std::string_view text, std::vector<std::string>& warnings) {
    Config cfg{};
    try {
        const auto root = toml::parse(text);

        cfg.schema_version = static_cast<int>(GetOr<int64_t>(root, "schema_version", 1));
        if (cfg.schema_version != 1) {
            warnings.push_back("Unknown config schema version " +
                               std::to_string(cfg.schema_version) +
                               "; ignoring unrecognised fields.");
        }

        if (auto t = root["general"].as_table()) {
            cfg.general.start_minimized   = GetOr<bool>(*t, "start_minimized", false);
            cfg.general.restore_last_mode = GetOr<bool>(*t, "restore_last_mode", true);
            cfg.general.active_priority   = GetOr<std::string>(*t, "active_priority", "above_normal");
        }
        if (auto t = root["lens"].as_table()) {
            cfg.lens.width            = static_cast<int>(GetOr<int64_t>(*t, "width",  640));
            cfg.lens.height           = static_cast<int>(GetOr<int64_t>(*t, "height", 360));
            cfg.lens.border_thickness = static_cast<int>(GetOr<int64_t>(*t, "border_thickness", 2));
            cfg.lens.border_color_hex = GetOr<std::string>(*t, "border_color", "#00E5FF");
            cfg.lens.follow_mouse     = GetOr<bool>(*t, "follow_mouse", true);
            cfg.lens.magnify_cursor   = GetOr<bool>(*t, "magnify_cursor", false);
            cfg.lens.position_tau     = static_cast<float>(GetOr<double>(*t, "position_tau", 0.05));
            cfg.lens.zoom_tau         = static_cast<float>(GetOr<double>(*t, "zoom_tau",     0.10));
        }
        if (auto t = root["zoom"].as_table()) {
            cfg.zoom.initial      = static_cast<float>(GetOr<double>(*t, "initial", 2.0));
            cfg.zoom.min          = static_cast<float>(GetOr<double>(*t, "min",     1.0));
            cfg.zoom.max          = static_cast<float>(GetOr<double>(*t, "max",    16.0));
            cfg.zoom.default_step = static_cast<float>(GetOr<double>(*t, "default_step", 0.25));
        }
        if (auto t = root["capture"].as_table()) {
            cfg.capture.mode = ParseCaptureMode(GetOr<std::string>(*t, "mode", "auto"));
        }
        if (auto t = root["ipc"].as_table()) {
            cfg.ipc.pipe_name = GetOr<std::string>(*t, "pipe_name", "MagnifierCtl");
            cfg.ipc.http_port = static_cast<int>(GetOr<int64_t>(*t, "http_port", 0));
        }
        if (auto t = root["advanced"].as_table()) {
            cfg.advanced.low_level_keyboard_hook = GetOr<bool>(*t, "low_level_keyboard_hook", false);
            cfg.advanced.log_level               = GetOr<std::string>(*t, "log_level", "info");
        }
        if (auto t = root["update"].as_table()) {
            cfg.update.check_on_startup = GetOr<bool>(*t, "check_on_startup", true);
            cfg.update.auto_download    = GetOr<bool>(*t, "auto_download",    false);
            cfg.update.owner            = GetOr<std::string>(*t, "owner", "");
            cfg.update.repo             = GetOr<std::string>(*t, "repo",  "Magnifier");
            cfg.update.token            = GetOr<std::string>(*t, "token", "");
        }
        if (auto t = root["hotkeys"].as_table()) {
            ParseHotkeyTable(*t, cfg, warnings);
        }
        if (auto t = root["controller"].as_table()) {
            ParseController(*t, cfg);
        }
    } catch (const toml::parse_error& e) {
        std::ostringstream oss;
        oss << "TOML parse error: " << e.description()
            << " (line " << e.source().begin.line << ")";
        warnings.push_back(oss.str());
        // Fall through with defaults.
    }
    return cfg;
}

} // namespace

LoadResult LoadConfig(const fs::path& path) {
    LoadResult res;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        // Materialise the default file so users have something to edit.
        std::ofstream out(path, std::ios::binary);
        if (out) {
            out << kEmbeddedDefaultToml;
        } else {
            res.warnings.push_back("Failed to create default config at " +
                WideToUtf8(path.wstring()));
        }
        res.config = ParseToml(kEmbeddedDefaultToml, res.warnings);
        res.loaded_from_file = false;
        return res;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        res.warnings.push_back("Failed to open config: " + WideToUtf8(path.wstring()));
        res.config = ParseToml(kEmbeddedDefaultToml, res.warnings);
        return res;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    res.config = ParseToml(ss.str(), res.warnings);
    res.loaded_from_file = true;
    return res;
}

bool SaveConfig(const fs::path& path, const Config& cfg) {
    // We re-serialise via toml++ to preserve types/comments minimally.
    toml::table root;
    root.insert("schema_version", static_cast<int64_t>(cfg.schema_version));

    toml::table g;
    g.insert("start_minimized",   cfg.general.start_minimized);
    g.insert("restore_last_mode", cfg.general.restore_last_mode);
    g.insert("active_priority",   cfg.general.active_priority);
    root.insert("general", std::move(g));

    toml::table l;
    l.insert("width",            static_cast<int64_t>(cfg.lens.width));
    l.insert("height",           static_cast<int64_t>(cfg.lens.height));
    l.insert("border_thickness", static_cast<int64_t>(cfg.lens.border_thickness));
    l.insert("border_color",     cfg.lens.border_color_hex);
    l.insert("follow_mouse",     cfg.lens.follow_mouse);
    l.insert("magnify_cursor",   cfg.lens.magnify_cursor);
    l.insert("position_tau",     static_cast<double>(cfg.lens.position_tau));
    l.insert("zoom_tau",         static_cast<double>(cfg.lens.zoom_tau));
    root.insert("lens", std::move(l));

    toml::table z;
    z.insert("initial",      static_cast<double>(cfg.zoom.initial));
    z.insert("min",          static_cast<double>(cfg.zoom.min));
    z.insert("max",          static_cast<double>(cfg.zoom.max));
    z.insert("default_step", static_cast<double>(cfg.zoom.default_step));
    root.insert("zoom", std::move(z));

    toml::table c;
    const char* mode_str =
        cfg.capture.mode == CaptureMode::ExcludeSelf ? "exclude_self" :
        cfg.capture.mode == CaptureMode::IncludeSelf ? "include_self" : "auto";
    c.insert("mode", mode_str);
    root.insert("capture", std::move(c));

    toml::table ip;
    ip.insert("pipe_name", cfg.ipc.pipe_name);
    ip.insert("http_port", static_cast<int64_t>(cfg.ipc.http_port));
    root.insert("ipc", std::move(ip));

    toml::table a;
    a.insert("low_level_keyboard_hook", cfg.advanced.low_level_keyboard_hook);
    a.insert("log_level",               cfg.advanced.log_level);
    root.insert("advanced", std::move(a));

    toml::table u;
    u.insert("check_on_startup", cfg.update.check_on_startup);
    u.insert("auto_download",    cfg.update.auto_download);
    u.insert("owner",            cfg.update.owner);
    u.insert("repo",             cfg.update.repo);
    u.insert("token",            cfg.update.token);
    root.insert("update", std::move(u));

    toml::table hk;
    auto put_hotkey = [&](std::string_view name, Action act) {
        auto it = cfg.hotkeys.find(act);
        if (it != cfg.hotkeys.end() && it->second.is_bound()) {
            std::string spec;
            if (it->second.modifiers & MOD_CONTROL) spec += "ctrl+";
            if (it->second.modifiers & MOD_ALT)     spec += "alt+";
            if (it->second.modifiers & MOD_SHIFT)   spec += "shift+";
            if (it->second.modifiers & MOD_WIN)     spec += "win+";
            spec += NameFromVk(it->second.vk);
            hk.insert(name, spec);
        } else {
            hk.insert(name, "");
        }
    };
    put_hotkey("toggle_lens",       Action::ToggleLens);
    put_hotkey("toggle_fullscreen", Action::ToggleFullscreen);
    put_hotkey("turn_off",          Action::TurnOff);
    put_hotkey("zoom_in",           Action::ZoomIn);
    put_hotkey("zoom_out",          Action::ZoomOut);
    put_hotkey("lens_size_up",      Action::LensSizeUp);
    put_hotkey("lens_size_down",    Action::LensSizeDown);
    put_hotkey("recenter",          Action::Recenter);
    root.insert("hotkeys", std::move(hk));

    toml::table ctrl;
    ctrl.insert("enabled",               cfg.controller.enabled);
    ctrl.insert("deadzone",              static_cast<double>(cfg.controller.deadzone));
    ctrl.insert("curve",                 static_cast<double>(cfg.controller.curve));
    ctrl.insert("move_speed",            static_cast<double>(cfg.controller.move_speed));
    ctrl.insert("zoom_speed",            static_cast<double>(cfg.controller.zoom_speed));
    ctrl.insert("idle_recenter_seconds", static_cast<double>(cfg.controller.idle_recenter_seconds));
    toml::table cb;
    cb.insert("move_x",            cfg.controller.bindings.move_x);
    cb.insert("move_y",            cfg.controller.bindings.move_y);
    cb.insert("zoom_axis",         cfg.controller.bindings.zoom_axis);
    cb.insert("toggle_lens",       cfg.controller.bindings.toggle_lens);
    cb.insert("toggle_fullscreen", cfg.controller.bindings.toggle_fullscreen);
    cb.insert("turn_off",          cfg.controller.bindings.turn_off);
    cb.insert("recenter",          cfg.controller.bindings.recenter);
    ctrl.insert("bindings", std::move(cb));
    root.insert("controller", std::move(ctrl));

    // Atomic write: temp + ReplaceFileW.
    const fs::path tmp = path;
    fs::path tmp_path = tmp;
    tmp_path += L".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << root;
        if (!out) return false;
    }

    std::error_code ec;
    if (fs::exists(path, ec)) {
        // ReplaceFileW preserves attributes/permissions atomically.
        if (!::ReplaceFileW(path.c_str(), tmp_path.c_str(),
                            nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                            nullptr, nullptr)) {
            // Fallback: rename + remove (still effectively atomic on NTFS).
            fs::remove(path, ec);
            fs::rename(tmp_path, path, ec);
            return !ec;
        }
        return true;
    }
    fs::rename(tmp_path, path, ec);
    return !ec;
}

} // namespace magnifier
