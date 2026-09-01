#include "Commands.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <dev/devs.hpp>

#include "Config.h"

using namespace std;

namespace cli {
namespace {

ostringstream g_out;

// --------------------------------------------------------------- diagnostics

int usageError(const string &msg)
{
    cerr << "obsbot-cli: " << msg << endl;
    return ExitUsage;
}

/// Report an SDK call. The SDK does not document its return codes as a stable
/// enum, so surface the raw number rather than inventing an interpretation.
int sdkResult(const string &what, int32_t ret)
{
    if (ret == 0) {
        out() << what << "\n";
        return ExitOk;
    }
    cerr << "obsbot-cli: " << what << " failed (SDK code " << ret << ")" << endl;
    return ExitDeviceError;
}

// ------------------------------------------------------------ argument types

enum class Switch { On, Off, Toggle, Invalid };

Switch parseSwitch(const string &s)
{
    if (s == "on" || s == "1" || s == "true" || s == "enable") return Switch::On;
    if (s == "off" || s == "0" || s == "false" || s == "disable") return Switch::Off;
    if (s == "toggle") return Switch::Toggle;
    return Switch::Invalid;
}

/// Strict numeric parsing: stod/stoi alone accept "1.5x" and "3junk", which
/// would silently turn a typo into a camera movement.
bool parseDouble(const string &s, double &value)
{
    try {
        size_t used = 0;
        value = stod(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

bool parseInt(const string &s, int &value)
{
    try {
        size_t used = 0;
        value = stoi(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

double clampRange(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------- device status

/// The SDK fills its status cache from an asynchronous device stream, so a
/// read taken right after enumeration can come back all zeros. A `toggle` that
/// trusted those zeros would flip the wrong way, so wait for the cache to look
/// real first. zoom_ratio is the tell: the camera reports 100..200 (1.0x-2.0x)
/// and never 0, so a zero means "not populated yet" rather than a real value.
bool readStatus(const shared_ptr<Device> &dev, Device::CameraStatus &st)
{
    for (int i = 0; i < 20; i++) {  // 20 x 100ms = 2s
        st = dev->cameraStatus();
        if (st.tiny.zoom_ratio >= 100 && st.tiny.zoom_ratio <= 200) return true;
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    cerr << "obsbot-cli: camera did not report its state within 2s;"
         << " toggle and relative commands need it, absolute ones do not" << endl;
    return false;
}

/// Shared shape of the boolean commands. Arguments are parsed before the
/// device is resolved, so `hdr maybe` fails instantly instead of after the
/// detection timeout; the status read happens only for `toggle`.
template <typename CurrentFn, typename ApplyFn>
int booleanCommand(const string &name, const vector<string> &args,
                   const DeviceProvider &device, CurrentFn current, ApplyFn apply)
{
    if (args.size() != 1) return usageError(name + ": expected on|off|toggle");

    Switch sw = parseSwitch(args[0]);
    if (sw == Switch::Invalid) {
        return usageError(name + ": expected on|off|toggle, got '" + args[0] + "'");
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    bool want = (sw == Switch::On);
    if (sw == Switch::Toggle) {
        Device::CameraStatus st;
        if (!readStatus(dev, st)) return ExitDeviceError;
        want = !current(st);
    }

    return sdkResult(name + " " + (want ? "on" : "off"), apply(dev, want));
}

// ------------------------------------------------------------------ commands

int cmdHdr(const DeviceProvider &device, Config &, const vector<string> &args)
{
    return booleanCommand("hdr", args, device,
        [](const Device::CameraStatus &st) { return st.tiny.hdr != 0; },
        [](const shared_ptr<Device> &dev, bool on) {
            return dev->cameraSetWdrR(on ? Device::DevWdrModeDol2TO1 : Device::DevWdrModeNone);
        });
}

int cmdFaceAe(const DeviceProvider &device, Config &, const vector<string> &args)
{
    return booleanCommand("face-ae", args, device,
        [](const Device::CameraStatus &st) { return st.tiny.face_ae != 0; },
        [](const shared_ptr<Device> &dev, bool on) { return dev->cameraSetFaceAER(on ? 1 : 0); });
}

int cmdFaceFocus(const DeviceProvider &device, Config &, const vector<string> &args)
{
    return booleanCommand("face-focus", args, device,
        [](const Device::CameraStatus &st) { return st.tiny.face_auto_focus != 0; },
        [](const shared_ptr<Device> &dev, bool on) { return dev->cameraSetFaceFocusR(on); });
}

/// Auto-framing is driven by media mode, which the tiny status block does not
/// report back -- unlike hdr or face_ae. So on|off work and toggle cannot;
/// saying so is better than toggling against a value we would have to guess.
int cmdTracking(const DeviceProvider &device, Config &, const vector<string> &args)
{
    if (args.size() != 1) return usageError("tracking: expected on|off");

    Switch sw = parseSwitch(args[0]);
    if (sw == Switch::Toggle) {
        return usageError("tracking: toggle is unsupported because the camera does not"
                          " report media mode; use 'ai toggle <mode>' instead");
    }
    if (sw == Switch::Invalid) {
        return usageError("tracking: expected on|off, got '" + args[0] + "'");
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    if (sw == Switch::Off) {
        return sdkResult("tracking off", dev->cameraSetMediaModeU(Device::MediaModeNormal));
    }

    int32_t ret = dev->cameraSetMediaModeU(Device::MediaModeAutoFrame);
    if (ret != 0) return sdkResult("tracking on", ret);
    // The device rejects a framing mode sent too soon after the mode switch.
    this_thread::sleep_for(chrono::milliseconds(500));
    return sdkResult("tracking on",
                     dev->cameraSetAutoFramingModeU(Device::AutoFrmSingle, Device::AutoFrmUpperBody));
}

struct AiModeName {
    const char *name;
    Device::AiWorkModeType mode;
};

const AiModeName kAiModes[] = {
    {"none",       Device::AiWorkModeNone},
    {"group",      Device::AiWorkModeGroup},
    {"single",     Device::AiWorkModeHuman},
    {"hand",       Device::AiWorkModeHand},
    {"whiteboard", Device::AiWorkModeWhiteBoard},
    {"desk",       Device::AiWorkModeDesk},
};

bool parseAiMode(const string &s, Device::AiWorkModeType &mode)
{
    for (const auto &m : kAiModes) {
        if (s == m.name) {
            mode = m.mode;
            return true;
        }
    }
    return false;
}

const char *aiModeName(uint8_t mode)
{
    for (const auto &m : kAiModes) {
        if (static_cast<uint8_t>(m.mode) == mode) return m.name;
    }
    return "unknown";
}

/// `ai <mode>` sets it outright. `ai toggle <mode>` is the Stream Deck shape:
/// one button that turns a mode on and turns it back off if already active. A
/// bare `ai toggle` has no meaning for a multi-valued setting.
int cmdAi(const DeviceProvider &device, Config &, const vector<string> &args)
{
    const string modes = "none|group|single|hand|whiteboard|desk";
    const bool toggling = args.size() == 2 && args[0] == "toggle";

    if (args.size() == 1 && args[0] == "toggle") {
        return usageError("ai: 'toggle' needs a mode, e.g. 'ai toggle single'");
    }
    if (args.size() != 1 && !toggling) {
        return usageError("ai: expected '<mode>' or 'toggle <mode>' where mode is " + modes);
    }

    const string &wanted = toggling ? args[1] : args[0];
    Device::AiWorkModeType mode;
    if (!parseAiMode(wanted, mode)) {
        return usageError(string(toggling ? "ai toggle" : "ai") + ": expected " + modes +
                          ", got '" + wanted + "'");
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    if (!toggling) return sdkResult("ai " + wanted, dev->cameraSetAiModeU(mode));

    Device::CameraStatus st;
    if (!readStatus(dev, st)) return ExitDeviceError;

    const bool active = st.tiny.ai_mode == static_cast<uint8_t>(mode);
    return sdkResult("ai " + string(active ? "none" : wanted),
                     dev->cameraSetAiModeU(active ? Device::AiWorkModeNone : mode));
}

int cmdFov(const DeviceProvider &device, Config &, const vector<string> &args)
{
    // Ordered so `cycle` walks wide -> medium -> narrow -> wide.
    const Device::FovType order[] = {Device::FovType86, Device::FovType78, Device::FovType65};
    const char *names[] = {"wide", "medium", "narrow"};

    if (args.size() != 1) return usageError("fov: expected wide|medium|narrow|cycle");

    const bool cycling = args[0] == "cycle";
    int index = -1;
    if (!cycling) {
        for (int i = 0; i < 3; i++) {
            if (args[0] == names[i]) index = i;
        }
        if (index < 0) {
            return usageError("fov: expected wide|medium|narrow|cycle, got '" + args[0] + "'");
        }
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    if (cycling) {
        Device::CameraStatus st;
        if (!readStatus(dev, st)) return ExitDeviceError;
        index = 0;
        for (int i = 0; i < 3; i++) {
            if (st.tiny.fov == static_cast<uint8_t>(order[i])) {
                index = (i + 1) % 3;
                break;
            }
        }
    }

    return sdkResult(string("fov ") + names[index], dev->cameraSetFovU(order[index]));
}

int cmdZoom(const DeviceProvider &device, Config &, const vector<string> &args)
{
    const double kMin = 1.0, kMax = 2.0;

    if (args.empty()) return usageError("zoom: expected <1.0-2.0>|in|out|reset");

    const bool relative = args[0] == "in" || args[0] == "out";
    double target = 0.0;
    double step = 0.1;

    if (args[0] == "reset") {
        if (args.size() != 1) return usageError("zoom: 'reset' takes no arguments");
        target = kMin;
    } else if (relative) {
        if (args.size() > 2) return usageError("zoom: expected 'in|out [step]'");
        if (args.size() == 2 && !parseDouble(args[1], step)) {
            return usageError("zoom: step must be a number, got '" + args[1] + "'");
        }
    } else {
        if (args.size() != 1) return usageError("zoom: expected a single value");
        if (!parseDouble(args[0], target)) {
            return usageError("zoom: expected a number in 1.0-2.0, got '" + args[0] + "'");
        }
        if (target < kMin || target > kMax) {
            return usageError("zoom: " + args[0] + " is outside the supported range 1.0-2.0");
        }
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    if (relative) {
        // Zoom, unlike pan/tilt, is reported by the camera, so relative moves
        // need no stored state.
        Device::CameraStatus st;
        if (!readStatus(dev, st)) return ExitDeviceError;
        const double current = st.tiny.zoom_ratio / 100.0;
        target = clampRange(args[0] == "in" ? current + step : current - step, kMin, kMax);
    }

    char label[32];
    snprintf(label, sizeof(label), "zoom %.2fx", target);
    return sdkResult(label, dev->cameraSetZoomAbsoluteR(static_cast<float>(target)));
}

/// Pan/tilt is the one setting the camera does not report back, so relative
/// moves have nothing to be relative to within a single process. The stored
/// position in the config file is the only continuity across the one-shot
/// invocations a Stream Deck makes, so relative moves read it and write it
/// back. Absolute moves persist too, to keep the stored value honest.
int cmdPanTilt(const DeviceProvider &device, Config &config, const vector<string> &args)
{
    if (args.empty()) {
        return usageError("pan-tilt: expected '<pan> <tilt>'|center|left|right|up|down");
    }

    const bool centering = args[0] == "center";
    const bool directional = args[0] == "left" || args[0] == "right" ||
                             args[0] == "up" || args[0] == "down";
    double step = 0.1;
    double absPan = 0.0, absTilt = 0.0;

    if (centering) {
        if (args.size() != 1) return usageError("pan-tilt: 'center' takes no arguments");
    } else if (directional) {
        if (args.size() > 2) return usageError("pan-tilt: expected '<direction> [step]'");
        if (args.size() == 2 && !parseDouble(args[1], step)) {
            return usageError("pan-tilt: step must be a number, got '" + args[1] + "'");
        }
    } else {
        if (args.size() != 2) {
            return usageError("pan-tilt: expected '<pan> <tilt>', both -1.0 to 1.0");
        }
        if (!parseDouble(args[0], absPan) || !parseDouble(args[1], absTilt)) {
            return usageError("pan-tilt: pan and tilt must be numbers");
        }
        if (absPan < -1.0 || absPan > 1.0 || absTilt < -1.0 || absTilt > 1.0) {
            return usageError("pan-tilt: values must be within -1.0 to 1.0");
        }
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    Config::CameraSettings settings = config.getSettings();
    double pan = settings.pan;
    double tilt = settings.tilt;

    if (centering) {
        pan = 0.0;
        tilt = 0.0;
    } else if (directional) {
        if (args[0] == "left")  pan  = clampRange(pan - step, -1.0, 1.0);
        if (args[0] == "right") pan  = clampRange(pan + step, -1.0, 1.0);
        if (args[0] == "up")    tilt = clampRange(tilt + step, -1.0, 1.0);
        if (args[0] == "down")  tilt = clampRange(tilt - step, -1.0, 1.0);
    } else {
        pan = absPan;
        tilt = absTilt;
    }

    char label[48];
    snprintf(label, sizeof(label), "pan-tilt %.2f %.2f", pan, tilt);

    int32_t ret = dev->cameraSetPanTiltAbsolute(pan, tilt);
    if (ret != 0) return sdkResult(label, ret);

    settings.pan = pan;
    settings.tilt = tilt;
    config.setSettings(settings);
    if (config.isSavingEnabled() && !config.save()) {
        // The camera did move, so this is a warning, not a failure: only the
        // next relative move is affected.
        cerr << "obsbot-cli: warning: could not save position to " << config.getConfigPath()
             << "; the next relative move will start from the stored value" << endl;
    }
    return sdkResult(label, 0);
}

int cmdFocus(const DeviceProvider &device, Config &, const vector<string> &args)
{
    if (args.empty()) return usageError("focus: expected auto|<0-100>|in|out");

    const bool automatic = args[0] == "auto";
    const bool relative = args[0] == "in" || args[0] == "out";
    int step = 5;
    int target = 0;

    if (automatic) {
        if (args.size() != 1) return usageError("focus: 'auto' takes no arguments");
    } else if (relative) {
        if (args.size() > 2) return usageError("focus: expected 'in|out [step]'");
        if (args.size() == 2 && !parseInt(args[1], step)) {
            return usageError("focus: step must be a whole number, got '" + args[1] + "'");
        }
    } else {
        if (args.size() != 1) return usageError("focus: expected a single value");
        if (!parseInt(args[0], target)) {
            return usageError("focus: expected auto or a whole number 0-100, got '" + args[0] + "'");
        }
        if (target < 0 || target > 100) {
            return usageError("focus: " + args[0] + " is outside the supported range 0-100");
        }
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    if (automatic) return sdkResult("focus auto", dev->cameraSetFocusAbsolute(0, true));

    if (relative) {
        Device::CameraStatus st;
        if (!readStatus(dev, st)) return ExitDeviceError;
        const int current = st.tiny.manual_focus_value;
        target = static_cast<int>(clampRange(args[0] == "in" ? current + step : current - step, 0, 100));
    }

    return sdkResult("focus " + to_string(target), dev->cameraSetFocusAbsolute(target, false));
}

/// Presets are the config file's three pan/tilt/zoom slots, the same ones the
/// GUI writes, so a slot saved in the GUI recalls from a Stream Deck button.
int cmdPreset(const DeviceProvider &device, Config &config, const vector<string> &args)
{
    const bool saving = !args.empty() && args[0] == "save";
    const size_t slotIndex = saving ? 1 : 0;

    if (args.size() != slotIndex + 1) return usageError("preset: expected '[save] 1|2|3'");

    int slot = 0;
    if (!parseInt(args[slotIndex], slot) || slot < 1 || slot > 3) {
        return usageError("preset: slot must be 1, 2 or 3, got '" + args[slotIndex] + "'");
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    Config::CameraSettings settings = config.getSettings();
    Config::CameraSettings::PresetSlot &preset = settings.presets[slot - 1];

    if (saving) {
        // Pan/tilt come from the stored position rather than the camera, which
        // does not report them; zoom is read back so a manual zoom is captured.
        Device::CameraStatus st;
        if (!readStatus(dev, st)) return ExitDeviceError;

        preset.defined = true;
        preset.pan = settings.pan;
        preset.tilt = settings.tilt;
        preset.zoom = st.tiny.zoom_ratio / 100.0;
        config.setSettings(settings);
        if (!config.isSavingEnabled() || !config.save()) {
            cerr << "obsbot-cli: could not write preset to " << config.getConfigPath() << endl;
            return ExitDeviceError;
        }
        out() << "preset " << slot << " saved\n";
        return ExitOk;
    }

    if (!preset.defined) {
        cerr << "obsbot-cli: preset " << slot << " is not set; save one with 'preset save "
             << slot << "'" << endl;
        return ExitUsage;
    }

    int32_t ret = dev->cameraSetPanTiltAbsolute(preset.pan, preset.tilt);
    if (ret != 0) return sdkResult("preset " + to_string(slot), ret);
    ret = dev->cameraSetZoomAbsoluteR(static_cast<float>(preset.zoom));
    if (ret != 0) return sdkResult("preset " + to_string(slot), ret);

    settings.pan = preset.pan;
    settings.tilt = preset.tilt;
    settings.zoom = preset.zoom;
    config.setSettings(settings);
    if (config.isSavingEnabled()) config.save();

    out() << "preset " << slot << " recalled\n";
    return ExitOk;
}

/// Machine-readable on purpose: `key=value` lines survive grep/cut in a Stream
/// Deck button script far better than the interactive menu's prose.
int cmdStatus(const DeviceProvider &device, Config &, const vector<string> &args)
{
    if (!args.empty()) return usageError("status: takes no arguments");

    auto dev = device();
    if (!dev) return ExitNoDevice;

    Device::CameraStatus st;
    if (!readStatus(dev, st)) return ExitDeviceError;

    const char *fovNames[] = {"wide", "medium", "narrow"};

    out() << "name=" << dev->devName() << "\n"
          << "serial=" << dev->devSn() << "\n"
          << "version=" << dev->devVersion() << "\n"
          << "zoom=" << (st.tiny.zoom_ratio / 100.0) << "\n"
          << "hdr=" << (st.tiny.hdr ? "on" : "off") << "\n"
          << "face_ae=" << (st.tiny.face_ae ? "on" : "off") << "\n"
          << "face_focus=" << (st.tiny.face_auto_focus ? "on" : "off") << "\n"
          << "auto_focus=" << (st.tiny.auto_focus ? "on" : "off") << "\n"
          << "focus=" << static_cast<int>(st.tiny.manual_focus_value) << "\n"
          << "fov=" << (st.tiny.fov < 3 ? fovNames[st.tiny.fov] : "unknown") << "\n"
          << "ai_mode=" << aiModeName(st.tiny.ai_mode) << "\n";
    return ExitOk;
}

// ------------------------------------------------------------- command table

struct Command {
    const char *name;
    const char *args;
    const char *summary;
    int (*handler)(const DeviceProvider &, Config &, const vector<string> &);
};

const Command kCommands[] = {
    {"hdr",        "on|off|toggle",                 "HDR (wide dynamic range)",             cmdHdr},
    {"tracking",   "on|off",                        "auto-framing (media mode)",            cmdTracking},
    {"ai",         "<mode>|toggle <mode>",          "none group single hand whiteboard desk", cmdAi},
    {"fov",        "wide|medium|narrow|cycle",      "field of view (86/78/65 degrees)",     cmdFov},
    {"zoom",       "<1.0-2.0>|in|out|reset [step]", "zoom level",                           cmdZoom},
    {"pan-tilt",   "<pan> <tilt>|center|<dir>",     "gimbal; dir = left right up down",     cmdPanTilt},
    {"focus",      "auto|<0-100>|in|out [step]",    "focus",                                cmdFocus},
    {"face-ae",    "on|off|toggle",                 "face auto-exposure",                   cmdFaceAe},
    {"face-focus", "on|off|toggle",                 "face auto-focus",                      cmdFaceFocus},
    {"preset",     "[save] 1|2|3",                  "recall or store a pan/tilt/zoom slot", cmdPreset},
    {"status",     "",                              "print current state as key=value",     cmdStatus},
};

}  // namespace

ostream &out() { return g_out; }

void writeOutput(int fd)
{
    const string s = g_out.str();
    if (s.empty()) return;
    // Deliberately write(2) rather than cout: fd 1 has been repointed at
    // stderr to keep the SDK's logging out of the caller's stdout, so the
    // saved descriptor is the only route back to it.
    size_t written = 0;
    while (written < s.size()) {
        ssize_t n = write(fd, s.data() + written, s.size() - written);
        if (n <= 0) return;
        written += static_cast<size_t>(n);
    }
}

bool isCommand(const string &name)
{
    for (const auto &c : kCommands) {
        if (name == c.name) return true;
    }
    return false;
}

int run(const DeviceProvider &device, Config &config,
        const string &name, const vector<string> &args)
{
    for (const auto &c : kCommands) {
        if (name == c.name) return c.handler(device, config, args);
    }
    return usageError("unknown command '" + name + "'; run --help for the list");
}

void printUsage()
{
    cout << "Commands (one-shot; exit 0 = applied, 1 = device error, 2 = usage, 3 = no camera):\n";
    for (const auto &c : kCommands) {
        const string invocation = string(c.name) + " " + c.args;
        cout << "  " << invocation;
        for (size_t i = invocation.size(); i < 34; i++) cout << ' ';
        cout << c.summary << "\n";
    }
    cout << "\nExamples:\n"
         << "  obsbot-cli hdr toggle\n"
         << "  obsbot-cli ai toggle single\n"
         << "  obsbot-cli zoom in 0.25\n"
         << "  obsbot-cli preset 2\n";
}

}  // namespace cli
