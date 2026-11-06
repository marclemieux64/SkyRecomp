#include "vr_settings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "vr_hud.h"


namespace {
constexpr VRConfig kDefaults = {1.0f, true, false, 0.7f, 100.0f, 0, 0.0f, 0.0f, true, 0};

std::string trim(std::string value) {
    const auto non_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(), value.end());
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool parse_float(const std::string &text, float &out) {
    char *end = nullptr;
    errno = 0;
    const float value = std::strtof(text.c_str(), &end);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (errno || end == text.c_str() || !end || *end || !std::isfinite(value)) return false;
    out = value;
    return true;
}

bool parse_int(const std::string &text, int &out) {
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (errno || end == text.c_str() || !end || *end) return false;
    out = static_cast<int>(value);
    return true;
}

bool parse_bool(const std::string &text, bool &out) {
    const std::string value = lowercase(trim(text));
    if (value == "1" || value == "true" || value == "yes" || value == "on") { out = true; return true; }
    if (value == "0" || value == "false" || value == "no" || value == "off") { out = false; return true; }
    return false;
}

void validate(VRConfig &config) {
    if (!std::isfinite(config.resolution_scale)) config.resolution_scale = kDefaults.resolution_scale;
    if (!std::isfinite(config.dynamic_resolution_min)) config.dynamic_resolution_min = kDefaults.dynamic_resolution_min;
    if (!std::isfinite(config.world_scale)) config.world_scale = kDefaults.world_scale;
    if (!std::isfinite(config.camera_height_offset)) config.camera_height_offset = kDefaults.camera_height_offset;
    if (!std::isfinite(config.camera_distance_offset)) config.camera_distance_offset = kDefaults.camera_distance_offset;
    config.resolution_scale = std::clamp(config.resolution_scale, 0.5f, 2.0f);
    config.dynamic_resolution_min = std::clamp(config.dynamic_resolution_min, 0.5f, config.resolution_scale);
    config.world_scale = std::clamp(config.world_scale, 50.0f, 200.0f);
    config.camera_height_offset = std::clamp(config.camera_height_offset, -50.0f, 50.0f);
    config.camera_distance_offset = std::clamp(config.camera_distance_offset, -100.0f, 100.0f);
    if (config.view_mode != 0 && config.view_mode != 1) config.view_mode = kDefaults.view_mode;
    if (config.move_direction < 0 || config.move_direction > 2) config.move_direction = kDefaults.move_direction;
    if (config.turn_mode < 0 || config.turn_mode > 2) config.turn_mode = kDefaults.turn_mode;
    if (config.door_mode < 0 || config.door_mode > 2) config.door_mode = kDefaults.door_mode;
    if (!std::isfinite(config.snap_turn_angle)) config.snap_turn_angle = kDefaults.snap_turn_angle;
    if (!std::isfinite(config.smooth_turn_speed)) config.smooth_turn_speed = kDefaults.smooth_turn_speed;
    config.snap_turn_angle = std::clamp(config.snap_turn_angle, 15.0f, 90.0f);
    config.smooth_turn_speed = std::clamp(config.smooth_turn_speed, 30.0f, 360.0f);
}

bool is_section(const std::string &line, const char *name) {
    const std::string value = trim(line);
    return value.size() >= 2 && value.front() == '[' && value.back() == ']' &&
           lowercase(trim(value.substr(1, value.size() - 2))) == lowercase(name);
}

bool any_section(const std::string &line) {
    const std::string value = trim(line);
    return value.size() >= 2 && value.front() == '[' && value.back() == ']';
}

std::vector<std::string> vr_lines() {
    std::ostringstream resolution, resolution_min, world, height, distance, hud_distance, hud_scale, hud_offset,
        wrist_size, wrist_side, wrist_up, wrist_forward, wrist_tilt, wrist_background, menu_distance, menu_width, menu_height;
    menu_distance << std::fixed << std::setprecision(2) << g_vr_hud_config.menu_distance;
    menu_width << std::fixed << std::setprecision(2) << g_vr_hud_config.menu_width;
    menu_height << std::fixed << std::setprecision(2) << g_vr_hud_config.menu_height;
    hud_distance << std::fixed << std::setprecision(2) << g_vr_hud_config.hud_distance;
    hud_scale << std::fixed << std::setprecision(2) << g_vr_hud_config.hud_scale;
    hud_offset << std::fixed << std::setprecision(2) << g_vr_hud_config.hud_vertical_offset;
    wrist_size << std::fixed << std::setprecision(2) << g_vr_hud_config.wrist_hud_size;
    wrist_side << std::fixed << std::setprecision(2) << g_vr_hud_config.wrist_hud_side;
    wrist_up << std::fixed << std::setprecision(2) << g_vr_hud_config.wrist_hud_up;
    wrist_forward << std::fixed << std::setprecision(2) << g_vr_hud_config.wrist_hud_forward;
    wrist_tilt << std::fixed << std::setprecision(1) << g_vr_hud_config.wrist_hud_tilt;
    wrist_background << std::fixed << std::setprecision(2) << g_vr_hud_config.wrist_hud_background;
    resolution << std::fixed << std::setprecision(2) << g_vr_config.resolution_scale;
    resolution_min << std::fixed << std::setprecision(2) << g_vr_config.dynamic_resolution_min;
    world << std::fixed << std::setprecision(2) << g_vr_config.world_scale;
    height << std::fixed << std::setprecision(2) << g_vr_config.camera_height_offset;
    distance << std::fixed << std::setprecision(2) << g_vr_config.camera_distance_offset;
    return {"[VR]", "resolution_scale = " + resolution.str(),
            std::string("desktop_mirror = ") + (g_vr_config.desktop_mirror ? "true" : "false"),
            std::string("dynamic_resolution = ") + (g_vr_config.dynamic_resolution ? "true" : "false"),
            "dynamic_resolution_min = " + resolution_min.str(),
            "world_scale = " + world.str(),
            "view_mode = " + std::to_string(g_vr_config.view_mode),
            "move_direction = " + std::to_string(g_vr_config.move_direction),
            "turn_mode = " + std::to_string(g_vr_config.turn_mode),
            "door_mode = " + std::to_string(g_vr_config.door_mode),
            std::string("sword_hitbox = ") + (g_vr_config.sword_hitbox ? "true" : "false"),
            "snap_turn_angle = " + std::to_string((int)g_vr_config.snap_turn_angle),
            "smooth_turn_speed = " + std::to_string((int)g_vr_config.smooth_turn_speed),
            "camera_height_offset = " + height.str(), "camera_distance_offset = " + distance.str(),
            std::string("level_horizon = ") + (g_vr_config.level_horizon ? "true" : "false"),
            "hud_distance = " + hud_distance.str(), "hud_scale = " + hud_scale.str(),
            "hud_vertical_offset = " + hud_offset.str(),
            std::string("hud_curvature = ") + (g_vr_hud_config.curvature ? "true" : "false"),
            std::string("hud_lock_to_head = ") + (g_vr_hud_config.lock_to_head ? "true" : "false"),
            std::string("wrist_hud = ") + (g_vr_hud_config.wrist_hud ? "true" : "false"),
            std::string("wrist_hud_glance = ") + (g_vr_hud_config.wrist_hud_glance ? "true" : "false"),
            std::string("wrist_hud_swap_hands = ") + (g_vr_hud_config.wrist_hud_swap_hands ? "true" : "false"),
            std::string("aim_reticle_3d = ") + (g_vr_hud_config.aim_reticle_3d ? "true" : "false"),
            std::string("aim_with_controller = ") + (g_vr_hud_config.aim_with_controller ? "true" : "false"),
            "wrist_hud_size = " + wrist_size.str(), "wrist_hud_side = " + wrist_side.str(),
            "wrist_hud_up = " + wrist_up.str(), "wrist_hud_forward = " + wrist_forward.str(),
            "wrist_hud_tilt = " + wrist_tilt.str(), "wrist_hud_background = " + wrist_background.str(),
            "menu_distance = " + menu_distance.str(), "menu_width = " + menu_width.str(),
            "menu_height = " + menu_height.str()};
}
} // namespace

VRConfig g_vr_config = kDefaults;

extern "C" void ResetVRConfig(void) { g_vr_config = kDefaults; }

extern "C" void LoadVRConfig(const char *filepath) {
    ResetVRConfig();
    ResetVRHUDConfig();
    if (!filepath || !*filepath) return;
    std::ifstream file(filepath);
    if (!file) return;

    bool in_vr = false;
    std::string line;
    while (std::getline(file, line)) {
        if (any_section(line)) { in_vr = is_section(line, "vr"); continue; }
        if (!in_vr) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = lowercase(trim(line.substr(0, equals)));
        std::string value = trim(line.substr(equals + 1));
        const std::size_t comment = value.find_first_of(";#");
        if (comment != std::string::npos) value = trim(value.substr(0, comment));
        if (key == "resolution_scale") parse_float(value, g_vr_config.resolution_scale);
        else if (key == "desktop_mirror") parse_bool(value, g_vr_config.desktop_mirror);
        else if (key == "dynamic_resolution") parse_bool(value, g_vr_config.dynamic_resolution);
        else if (key == "dynamic_resolution_min") parse_float(value, g_vr_config.dynamic_resolution_min);
        else if (key == "world_scale") parse_float(value, g_vr_config.world_scale);
        else if (key == "view_mode") parse_int(value, g_vr_config.view_mode);
        else if (key == "move_direction") parse_int(value, g_vr_config.move_direction);
        else if (key == "turn_mode") parse_int(value, g_vr_config.turn_mode);
        else if (key == "door_mode") parse_int(value, g_vr_config.door_mode);
        else if (key == "sword_hitbox") parse_bool(value, g_vr_config.sword_hitbox);
        else if (key == "snap_turn_angle") parse_float(value, g_vr_config.snap_turn_angle);
        else if (key == "smooth_turn_speed") parse_float(value, g_vr_config.smooth_turn_speed);
        else if (key == "camera_height_offset") parse_float(value, g_vr_config.camera_height_offset);
        else if (key == "camera_distance_offset") parse_float(value, g_vr_config.camera_distance_offset);
        else if (key == "level_horizon") parse_bool(value, g_vr_config.level_horizon);
        else if (key == "hud_distance") parse_float(value, g_vr_hud_config.hud_distance);
        else if (key == "hud_scale") parse_float(value, g_vr_hud_config.hud_scale);
        else if (key == "hud_vertical_offset") parse_float(value, g_vr_hud_config.hud_vertical_offset);
        else if (key == "hud_curvature") parse_bool(value, g_vr_hud_config.curvature);
        else if (key == "hud_lock_to_head") parse_bool(value, g_vr_hud_config.lock_to_head);
        else if (key == "wrist_hud") parse_bool(value, g_vr_hud_config.wrist_hud);
        else if (key == "wrist_hud_glance") parse_bool(value, g_vr_hud_config.wrist_hud_glance);
        else if (key == "wrist_hud_swap_hands") parse_bool(value, g_vr_hud_config.wrist_hud_swap_hands);
        else if (key == "aim_reticle_3d") parse_bool(value, g_vr_hud_config.aim_reticle_3d);
        else if (key == "aim_with_controller") parse_bool(value, g_vr_hud_config.aim_with_controller);
        else if (key == "wrist_hud_size") parse_float(value, g_vr_hud_config.wrist_hud_size);
        else if (key == "wrist_hud_side") parse_float(value, g_vr_hud_config.wrist_hud_side);
        else if (key == "wrist_hud_up") parse_float(value, g_vr_hud_config.wrist_hud_up);
        else if (key == "wrist_hud_forward") parse_float(value, g_vr_hud_config.wrist_hud_forward);
        else if (key == "wrist_hud_tilt") parse_float(value, g_vr_hud_config.wrist_hud_tilt);
        else if (key == "wrist_hud_background") parse_float(value, g_vr_hud_config.wrist_hud_background);
        else if (key == "menu_distance") parse_float(value, g_vr_hud_config.menu_distance);
        else if (key == "menu_width") parse_float(value, g_vr_hud_config.menu_width);
        else if (key == "menu_height") parse_float(value, g_vr_hud_config.menu_height);
    }
    validate(g_vr_config);
    ValidateVRHUDConfig();
}

extern "C" void SaveVRConfig(const char *filepath) {
    if (!filepath || !*filepath) return;
    validate(g_vr_config);
    ValidateVRHUDConfig();

    std::vector<std::string> input;
    std::ifstream source(filepath);
    std::string line;
    while (source && std::getline(source, line)) input.push_back(line);

    std::vector<std::string> output;
    bool replaced = false;
    for (std::size_t i = 0; i < input.size();) {
        if (!is_section(input[i], "vr")) { output.push_back(input[i++]); continue; }
        if (!replaced) {
            const auto section = vr_lines();
            output.insert(output.end(), section.begin(), section.end());
            replaced = true;
        }
        ++i;
        while (i < input.size() && !any_section(input[i])) ++i;
    }
    if (!replaced) {
        if (!output.empty() && !output.back().empty()) output.emplace_back();
        const auto section = vr_lines();
        output.insert(output.end(), section.begin(), section.end());
    }

    std::ofstream destination(filepath, std::ios::trunc);
    if (!destination) return;
    for (const auto &entry : output) destination << entry << '\n';
}

