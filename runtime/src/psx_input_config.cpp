/* Controller configuration -- see psx_input_config.h.
 *
 * The button-mapping tables and the input.ini parser that fills them. This is
 * the half of the input path that has no idea a player slot exists: it turns
 * text into a ControllerMap, and the sampling code in main.cpp reads that map
 * when it turns live devices into a PSX pad word.
 *
 * Per-GUID maps exist because two different pads plugged in at once need
 * different mappings, and the GUID is the only stable identity SDL offers.
 */

#include "psx_input_config.h"

#include "psx_sdl.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>

/* This module's own ini lexing. Deliberately private copies rather than a
 * shared string utility: main.cpp's trim/lower serve its .env and device-name
 * parsing too, and a header exporting "trim a string" would be a worse seam
 * than six duplicated lines. */
static std::string ini_trim(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) first++;
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) last--;
    return s.substr(first, last - first);
}

static std::string ini_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}



int controller_device_index = 0;
/* Default ~10% of SDL axis range (32767). Overridden per-player via settings. */
int controller_deadzone = 3277;
/* [controller] anti_deadzone (game.toml). 0 = off, the historical behaviour. */
int controller_anti_deadzone = 0;



ControllerMap controller_map = {{
    { PAD_UP,       "up",       {}, 0 },
    { PAD_DOWN,     "down",     {}, 0 },
    { PAD_LEFT,     "left",     {}, 0 },
    { PAD_RIGHT,    "right",    {}, 0 },
    { PAD_CROSS,    "cross",    {}, 0 },
    { PAD_CIRCLE,   "circle",   {}, 0 },
    { PAD_SQUARE,   "square",   {}, 0 },
    { PAD_TRIANGLE, "triangle", {}, 0 },
    { PAD_L1,       "l1",       {}, 0 },
    { PAD_R1,       "r1",       {}, 0 },
    { PAD_L2,       "l2",       {}, 0 },
    { PAD_R2,       "r2",       {}, 0 },
    { PAD_L3,       "l3",       {}, 0 },
    { PAD_R3,       "r3",       {}, 0 },
    { PAD_START,    "start",    {}, 0 },
    { PAD_SELECT,   "select",   {}, 0 },
    /* Stick directions (analog axes by default; fold onto d-pad digitally). */
    { 0, "ls_up",    {}, PAD_UP },
    { 0, "ls_down",  {}, PAD_DOWN },
    { 0, "ls_left",  {}, PAD_LEFT },
    { 0, "ls_right", {}, PAD_RIGHT },
    { 0, "rs_up",    {}, 0 },
    { 0, "rs_down",  {}, 0 },
    { 0, "rs_left",  {}, 0 },
    { 0, "rs_right", {}, 0 },
}};
/* Per-GUID overrides from input.ini [mapping.<guid>]. Absent GUID => global. */
std::unordered_map<std::string, ControllerMap> controller_maps_by_guid;

/* Takes the GUID rather than a PlayerInput: this module has no business
 * knowing what a player slot is, and the caller always has the string. */
const ControllerMap& controller_map_for(const char* guid) {
    if (guid && guid[0]) {
        auto it = controller_maps_by_guid.find(guid);
        if (it != controller_maps_by_guid.end()) return it->second;
    }
    return controller_map;
}
static bool parse_bool_value(const std::string& value, bool fallback) {
    std::string v = ini_lower(ini_trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

static ControllerSource parse_controller_source(const std::string& raw) {
    std::string s = ini_lower(ini_trim(raw));
    ControllerSource out;
    if (s.empty() || s == "none" || s == "disabled") return out;

    // recomp-ui pad capture persists axes as "name+" / "name-" (see
    // source_from_bind). Defaults historically omit the suffix for triggers
    // ("lefttrigger"). Accept both; strip the sign before name lookup.
    int dir = 0; // -1, 0 (unspecified), +1
    if (s.size() >= 2) {
        const char last = s.back();
        const char prev = s[s.size() - 2];
        if ((last == '+' || last == '-') &&
            (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')) {
            dir = (last == '+') ? +1 : -1;
            s.pop_back();
        }
    }

    auto as_button = [&](SDL_GameControllerButton b) -> ControllerSource {
        out.kind = ControllerSource::Kind::Button;
        out.id = b;
        return out;
    };
    auto as_axis = [&](SDL_GameControllerAxis a, int d) -> ControllerSource {
        // Unspecified direction → positive (triggers / capture default).
        out.kind = (d < 0) ? ControllerSource::Kind::AxisNegative
                           : ControllerSource::Kind::AxisPositive;
        out.id = a;
        return out;
    };

    if (s == "a") return as_button(SDL_CONTROLLER_BUTTON_A);
    if (s == "b") return as_button(SDL_CONTROLLER_BUTTON_B);
    if (s == "x") return as_button(SDL_CONTROLLER_BUTTON_X);
    if (s == "y") return as_button(SDL_CONTROLLER_BUTTON_Y);
    if (s == "back" || s == "view" || s == "select")
        return as_button(SDL_CONTROLLER_BUTTON_BACK);
    if (s == "start" || s == "menu")
        return as_button(SDL_CONTROLLER_BUTTON_START);
    if (s == "guide") return as_button(SDL_CONTROLLER_BUTTON_GUIDE);
    if (s == "leftstick") return as_button(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (s == "rightstick") return as_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    // Shoulders are digital buttons. A trailing +/- from axis-style capture is
    // ignored so "leftshoulder+" still maps to L1 instead of becoming unbound.
    if (s == "leftshoulder" || s == "lb" || s == "l1")
        return as_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    if (s == "rightshoulder" || s == "rb" || s == "r1")
        return as_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    if (s == "dpup" || s == "dpadup")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    if (s == "dpdown" || s == "dpaddown")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    if (s == "dpleft" || s == "dpadleft")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    if (s == "dpright" || s == "dpadright")
        return as_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    // Triggers: default "lefttrigger" and capture "lefttrigger+" both work.
    if (s == "lefttrigger" || s == "lt" || s == "l2")
        return as_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, dir == 0 ? +1 : dir);
    if (s == "righttrigger" || s == "rt" || s == "r2")
        return as_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, dir == 0 ? +1 : dir);

    // Stick axes (suffix required unless using a directional alias).
    if (s == "leftx") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_LEFTX, dir);
    }
    if (s == "lefty") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_LEFTY, dir);
    }
    if (s == "rightx") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_RIGHTX, dir);
    }
    if (s == "righty") {
        if (dir == 0) return out;
        return as_axis(SDL_CONTROLLER_AXIS_RIGHTY, dir);
    }
    if (s == "lsright") return as_axis(SDL_CONTROLLER_AXIS_LEFTX, +1);
    if (s == "lsleft") return as_axis(SDL_CONTROLLER_AXIS_LEFTX, -1);
    if (s == "lsdown") return as_axis(SDL_CONTROLLER_AXIS_LEFTY, +1);
    if (s == "lsup") return as_axis(SDL_CONTROLLER_AXIS_LEFTY, -1);
    if (s == "rsright") return as_axis(SDL_CONTROLLER_AXIS_RIGHTX, +1);
    if (s == "rsleft") return as_axis(SDL_CONTROLLER_AXIS_RIGHTX, -1);
    if (s == "rsdown") return as_axis(SDL_CONTROLLER_AXIS_RIGHTY, +1);
    if (s == "rsup") return as_axis(SDL_CONTROLLER_AXIS_RIGHTY, -1);

    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(s.c_str());
    if (button != SDL_CONTROLLER_BUTTON_INVALID)
        return as_button(button);

    SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(s.c_str());
    if (axis != SDL_CONTROLLER_AXIS_INVALID)
        return as_axis(axis, dir == 0 ? +1 : dir);

    return out;
}

static std::vector<ControllerSource> parse_source_list(const std::string& value) {
    std::vector<ControllerSource> sources;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        ControllerSource source = parse_controller_source(item);
        if (source.kind != ControllerSource::Kind::None) {
            sources.push_back(source);
        }
    }
    return sources;
}

static void apply_sources_to_map(ControllerMap& map, const char* name,
                                 const char* sources) {
    for (auto& entry : map) {
        if (std::strcmp(entry.ini_name, name) == 0) {
            entry.sources = parse_source_list(sources);
            return;
        }
    }
}

static void set_default_controller_mapping_into(ControllerMap& map) {
    for (auto& entry : map) entry.sources.clear();
    /* D-pad and sticks are separate (launcher Gamepad Bindings layout). Digital
     * mode still folds ls_* onto d-pad bits via fold_bit. */
    apply_sources_to_map(map, "up",       "dpup");
    apply_sources_to_map(map, "down",     "dpdown");
    apply_sources_to_map(map, "left",     "dpleft");
    apply_sources_to_map(map, "right",    "dpright");
    apply_sources_to_map(map, "cross",    "a");
    apply_sources_to_map(map, "circle",   "b");
    apply_sources_to_map(map, "square",   "x");
    apply_sources_to_map(map, "triangle", "y");
    apply_sources_to_map(map, "l1",       "leftshoulder");
    apply_sources_to_map(map, "r1",       "rightshoulder");
    apply_sources_to_map(map, "l2",       "lefttrigger");
    apply_sources_to_map(map, "r2",       "righttrigger");
    apply_sources_to_map(map, "l3",       "leftstick");
    apply_sources_to_map(map, "r3",       "rightstick");
    apply_sources_to_map(map, "start",    "start");
    apply_sources_to_map(map, "select",   "back");
    apply_sources_to_map(map, "ls_up",    "lefty-");
    apply_sources_to_map(map, "ls_down",  "lefty+");
    apply_sources_to_map(map, "ls_left",  "leftx-");
    apply_sources_to_map(map, "ls_right", "leftx+");
    apply_sources_to_map(map, "rs_up",    "righty-");
    apply_sources_to_map(map, "rs_down",  "righty+");
    apply_sources_to_map(map, "rs_left",  "rightx-");
    apply_sources_to_map(map, "rs_right", "rightx+");
}

void set_default_controller_mapping(void) {
    set_default_controller_mapping_into(controller_map);
    controller_maps_by_guid.clear();
}

static std::string default_input_ini_text(void) {
    return
        "; PSXRecomp input mapping. PSX buttons are active when any listed source is pressed.\n"
        "; Sources use SDL/Xbox names: a,b,x,y,back,start,leftshoulder,rightshoulder,\n"
        "; lefttrigger[/+],righttrigger[/+],leftstick,rightstick (stick clicks -> L3/R3),\n"
        "; dpup,dpdown,dpleft,dpright,leftx-/leftx+/lefty-/lefty+.\n"
        "; Axis capture may append +/−; both forms are accepted. PSX slots are digital.\n"
        "; Optional per-device overrides: [mapping.<sdl-guid>].\n"
        "\n"
        "[controller]\n"
        "enabled = true\n"
        "device = 0\n"
        "deadzone = 3277\n"
        "\n"
        "[mapping]\n"
        "up = dpup\n"
        "down = dpdown\n"
        "left = dpleft\n"
        "right = dpright\n"
        "cross = a\n"
        "circle = b\n"
        "square = x\n"
        "triangle = y\n"
        "l1 = leftshoulder\n"
        "r1 = rightshoulder\n"
        "l2 = lefttrigger\n"
        "r2 = righttrigger\n"
        "l3 = leftstick\n"
        "r3 = rightstick\n"
        "start = start\n"
        "select = back\n"
        "ls_up = lefty-\n"
        "ls_down = lefty+\n"
        "ls_left = leftx-\n"
        "ls_right = leftx+\n"
        "rs_up = righty-\n"
        "rs_down = righty+\n"
        "rs_left = rightx-\n"
        "rs_right = rightx+\n";
}

void load_input_config(const std::filesystem::path& exe_dir) {
    set_default_controller_mapping();

    namespace fs = std::filesystem;
    fs::path config_path = exe_dir / "input.ini";
    std::error_code ec;
    if (!fs::exists(config_path, ec)) {
        std::ofstream out(config_path, std::ios::binary);
        if (out) out << default_input_ini_text();
        return;
    }

    std::ifstream in(config_path);
    if (!in) {
        return;
    }

    bool controller_enabled = true;
    std::string section;
    std::string line;
    ControllerMap* guid_target = nullptr;
    while (std::getline(in, line)) {
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.resize(comment);
        line = ini_trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = ini_lower(ini_trim(line.substr(1, line.size() - 2)));
            guid_target = nullptr;
            if (section.rfind("mapping.", 0) == 0) {
                const std::string guid = section.substr(8);
                if (!guid.empty()) {
                    /* Seed from the current global map (defaults + any
                     * [mapping] keys already parsed), then overlay GUID keys. */
                    if (!controller_maps_by_guid.count(guid))
                        controller_maps_by_guid[guid] = controller_map;
                    guid_target = &controller_maps_by_guid[guid];
                }
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = ini_lower(ini_trim(line.substr(0, eq)));
        std::string value = ini_trim(line.substr(eq + 1));
        if (section == "controller") {
            if (key == "enabled") {
                controller_enabled = parse_bool_value(value, controller_enabled);
            } else if (key == "device") {
                controller_device_index = std::max(0, std::atoi(value.c_str()));
            } else if (key == "deadzone") {
                controller_deadzone = std::max(0, std::min(32767, std::atoi(value.c_str())));
            }
        } else if (section == "mapping") {
            for (auto& entry : controller_map) {
                if (key == entry.ini_name) {
                    entry.sources = parse_source_list(value);
                    break;
                }
            }
        } else if (guid_target) {
            for (auto& entry : *guid_target) {
                if (key == entry.ini_name) {
                    entry.sources = parse_source_list(value);
                    break;
                }
            }
        }
    }

    if (!controller_enabled) {
        for (auto& entry : controller_map) entry.sources.clear();
        for (auto& kv : controller_maps_by_guid)
            for (auto& entry : kv.second) entry.sources.clear();
    }


}
