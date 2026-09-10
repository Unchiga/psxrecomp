/* Controller configuration: the button-mapping tables and the input.ini parser
 * that fills them.
 *
 * Deliberately knows nothing about player slots. main.cpp owns the live devices
 * (g_players, the SDL handles) and reads the map from here when it samples
 * them; keeping the two apart is what lets a mapping be parsed and tested
 * without a controller plugged in. */
#ifndef PSX_INPUT_CONFIG_H
#define PSX_INPUT_CONFIG_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/* PS1 digital pad button bits (active-low: 0=pressed, 1=released).
 * Bit 0 = SELECT, Bit 1 = L3, Bit 2 = R3, Bit 3 = START,
 * Bit 4 = UP, Bit 5 = RIGHT, Bit 6 = DOWN, Bit 7 = LEFT,
 * Bit 8 = L2, Bit 9 = R2, Bit 10 = L1, Bit 11 = R1,
 * Bit 12 = TRIANGLE, Bit 13 = CIRCLE, Bit 14 = CROSS, Bit 15 = SQUARE.
 * L3/R3 (stick clicks) exist on a DualShock only; the wire reports them like
 * Beetle's dualshock.cpp does — straight from the button word, no mode mask. */
#define PAD_SELECT   (1 << 0)
#define PAD_L3       (1 << 1)
#define PAD_R3       (1 << 2)
#define PAD_START    (1 << 3)
#define PAD_UP       (1 << 4)
#define PAD_RIGHT    (1 << 5)
#define PAD_DOWN     (1 << 6)
#define PAD_LEFT     (1 << 7)
#define PAD_L2       (1 << 8)
#define PAD_R2       (1 << 9)
#define PAD_L1       (1 << 10)
#define PAD_R1       (1 << 11)
#define PAD_TRIANGLE (1 << 12)
#define PAD_CIRCLE   (1 << 13)
#define PAD_CROSS    (1 << 14)
#define PAD_SQUARE   (1 << 15)

/* One physical input a PSX button can be bound to. */
struct ControllerSource {
    enum class Kind {
        None,
        Button,
        AxisPositive,
        AxisNegative,
    };

    Kind kind = Kind::None;
    int id = -1;
};

struct PsxButtonMap {
    uint16_t bit;               /* 0 = stick-direction slot (no digital bit alone) */
    const char* ini_name;
    std::vector<ControllerSource> sources;
    /* When set, a pressed stick-direction source also contributes this d-pad
     * bit in digital mode (ls_* -> Up/Down/Left/Right). */
    uint16_t fold_bit = 0;
};

static constexpr int kControllerMapN = 24;
static constexpr int kDefaultDeadzoneRaw = 3277;
using ControllerMap = std::array<PsxButtonMap, kControllerMapN>;

/* The default map, and the per-GUID overrides layered on top of it. */
extern ControllerMap controller_map;
extern std::unordered_map<std::string, ControllerMap> controller_maps_by_guid;

/* The map that applies to a device, by SDL GUID. Empty or unknown GUID falls
 * back to the default map. */
const ControllerMap& controller_map_for(const char* guid);

/* Resolved [controller] settings. deadzone is the default for slots that do
 * not override it; anti_deadzone is 0 (off) unless a game asks for it. */
extern int controller_deadzone;
extern int controller_anti_deadzone;
extern int controller_device_index;

/* Reset the map to the compiled-in defaults. */
void set_default_controller_mapping(void);

/* Load input.ini from exe_dir, writing the documented default file when it is
 * missing. keybinds.ini is a separate concern -- call psx_keybinds_init()
 * yourself. */
void load_input_config(const std::filesystem::path& exe_dir);

#endif /* PSX_INPUT_CONFIG_H */
