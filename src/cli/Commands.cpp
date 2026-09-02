#include "Commands.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <dev/devs.hpp>

#include "Config.h"
#include "V4l2Backend.h"

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

// ----------------------------------------------------------- v4l2 backend

/// The camera's V4L2 node, or nullptr if no OBSBOT node is present.
///
/// Preferred over the SDK for controls that standard UVC already covers. Two
/// reasons. It is the same path the SDK itself takes -- libdev.so's
/// UVCControllerControls table decodes as the standard Camera Terminal and
/// Processing Unit control set, not as vendor commands -- so nothing is lost.
/// And it is immediate, where resolving an SDK device costs a 10s detection
/// timeout that a camera the SDK cannot claim (the Tiny 3, see ADR-002) always
/// pays in full before failing. A Stream Deck button cannot wait 10s to zoom.
V4l2Backend *v4l2()
{
    static V4l2Backend backend;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string path = V4l2Backend::findObsbotDevice();
        if (!path.empty()) backend.open(path);
    }
    return backend.isOpen() ? &backend : nullptr;
}

/// Report a V4L2 write. Mirrors sdkResult()'s contract: ExitOk only when the
/// camera actually changed.
int v4l2Result(const string &what, bool ok)
{
    if (ok) {
        out() << what << "\n";
        return ExitOk;
    }
    cerr << "obsbot-cli: " << what << " failed (V4L2 ioctl refused)" << endl;
    return ExitDeviceError;
}

/// Snap @p raw into @p range, honouring the control's step. The kernel rejects
/// or silently rounds an off-step value, so do it here where the result can be
/// reported accurately.
int snapToRange(double raw, const V4l2Backend::ControlRange &range)
{
    if (raw < range.min) raw = range.min;
    if (raw > range.max) raw = range.max;
    const int step = range.step > 0 ? range.step : 1;
    const long long offset = static_cast<long long>(llround((raw - range.min) / step)) * step;
    int value = static_cast<int>(range.min + offset);
    if (value > range.max) value -= step;
    return value;
}

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

/// Which gesture the camera should stop reacting to.
///
/// The maintainer's problem is the camera enabling AI tracking on its own when
/// they move their hands while working. Target selection is the gesture that
/// most plausibly does that, so it is the default: disabling the master switch
/// would also kill gesture zoom, which is worth keeping.
///
/// Which of these the Tiny 3 actually honours is not settled -- the SDK's
/// @category for this family says "tail2 and later products", but that
/// annotation has already proven wrong in the permissive direction twice on
/// this camera (gimbalGetAttitudeInfoR and aiGetGimbalParaR both work here
/// despite not listing tiny3), so the only way to know is to ask the device.
/// aiSetGestureCtrlR(bool) is deliberately not used: the SDK marks it
/// deprecated and "no longer used".
struct GestureTarget {
    const char *name;
    Device::DevGestureParaType type;
    int individual;  /// gesture index for the older aiSetGestureCtrlIndividualR
};

const GestureTarget kGestureTargets[] = {
    {"target", Device::DevGestureParaTypeTargetSelection, 0},
    {"zoom",   Device::DevGestureParaTypeZoom,            1},
    {"all",    Device::DevGestureParaTypeGesture,        -1},
};

/// `gesture on|off|toggle [target|zoom|all]`.
///
/// Deliberately does NOT use booleanCommand(): that reads state through
/// readStatus(), which never succeeds on a Tiny 3 because it gates on a
/// zoom_ratio encoding this model does not use. Reading gesture state through
/// aiGetGestureParaR sidesteps that entirely, so `gesture toggle` works on a
/// camera where `hdr toggle` currently cannot.
int cmdGesture(const DeviceProvider &device, Config &, const vector<string> &args)
{
    if (args.empty() || args.size() > 2) {
        return usageError("gesture: expected 'on|off|toggle [target|zoom|all]'");
    }

    const Switch sw = parseSwitch(args[0]);
    if (sw == Switch::Invalid) {
        return usageError("gesture: expected on|off|toggle, got '" + args[0] + "'");
    }

    const GestureTarget *target = &kGestureTargets[0];
    if (args.size() == 2) {
        target = nullptr;
        for (const auto &g : kGestureTargets) {
            if (args[1] == g.name) target = &g;
        }
        if (!target) {
            return usageError("gesture: expected target|zoom|all, got '" + args[1] + "'");
        }
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    // Read first, both to support toggle and to report what we found: a
    // command that silently no-ops because the setting was already correct is
    // indistinguishable from one the camera ignored.
    bool current = false;
    const int32_t getRet = dev->aiGetGestureParaR(target->type, current);
    if (getRet != 0 && sw == Switch::Toggle) {
        cerr << "obsbot-cli: gesture: cannot toggle -- the camera did not report"
             << " its gesture state (SDK code " << getRet << ")" << endl;
        return ExitDeviceError;
    }

    const bool want = (sw == Switch::Toggle) ? !current : (sw == Switch::On);

    int32_t ret = dev->aiSetGestureParaR(target->type, want);
    const char *via = "aiSetGestureParaR";
    if (ret != 0 && target->individual >= 0) {
        // Fall back to the older per-gesture call. Which family this model
        // honours is exactly what needs recording, so name the one that won.
        ret = dev->aiSetGestureCtrlIndividualR(target->individual, want);
        via = "aiSetGestureCtrlIndividualR";
    }

    const string label = string("gesture ") + target->name + " " + (want ? "on" : "off");
    if (ret != 0) {
        cerr << "obsbot-cli: " << label << " failed (SDK code " << ret << ")" << endl;
        return ExitDeviceError;
    }

    // Read back rather than trusting the write: this camera has already been
    // observed accepting and echoing a pan/tilt command it never executed.
    bool after = want;
    const bool verified = dev->aiGetGestureParaR(target->type, after) == 0;
    out() << label;
    if (getRet == 0) out() << " (was " << (current ? "on" : "off") << ")";
    if (verified && after != want) {
        out() << " -- WARNING: camera still reports "
              << (after ? "on" : "off") << "\n";
        cerr << "obsbot-cli: gesture: the camera accepted the change but reports"
             << " the old value; it may not support this control" << endl;
        return ExitDeviceError;
    }
    out() << " [via " << via << "]\n";
    return ExitOk;
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

/// Zoom argument. `1.0`-`2.0` is the historical ratio form; a `%` suffix
/// addresses the device's own scale directly.
///
/// The two forms exist because `zoom_absolute` is a device-specific scale
/// (0-100 on the Tiny 3), not a magnification factor, and the SDK's 1.0x-2.0x
/// contract has no published mapping onto it. Rather than invent a
/// magnification figure the hardware never promised, the ratio form is defined
/// to span the device's full zoom range: 1.0 is fully wide, 2.0 fully tight.
/// On a camera whose V4L2 range is itself expressed as a ratio (min 100,
/// max 200) that mapping is the identity, so the historical meaning survives
/// where it was ever well defined.
struct ZoomArg {
    bool percent = false;
    double value = 0.0;
};

bool parseZoomArg(const string &s, ZoomArg &arg)
{
    if (!s.empty() && s.back() == '%') {
        arg.percent = true;
        return parseDouble(s.substr(0, s.size() - 1), arg.value);
    }
    arg.percent = false;
    return parseDouble(s, arg.value);
}

int cmdZoom(const DeviceProvider &device, Config &, const vector<string> &args)
{
    const double kMin = 1.0, kMax = 2.0;

    if (args.empty()) return usageError("zoom: expected <1.0-2.0>|<0-100>%|in|out|reset");

    const bool relative = args[0] == "in" || args[0] == "out";
    ZoomArg target;
    ZoomArg step;
    step.value = 0.1;

    if (args[0] == "reset") {
        if (args.size() != 1) return usageError("zoom: 'reset' takes no arguments");
        target.value = kMin;
    } else if (relative) {
        if (args.size() > 2) return usageError("zoom: expected 'in|out [step]'");
        if (args.size() == 2 && !parseZoomArg(args[1], step)) {
            return usageError("zoom: step must be a number, got '" + args[1] + "'");
        }
    } else {
        if (args.size() != 1) return usageError("zoom: expected a single value");
        if (!parseZoomArg(args[0], target)) {
            return usageError("zoom: expected a number in 1.0-2.0 or 0-100%, got '" +
                              args[0] + "'");
        }
        if (target.percent) {
            if (target.value < 0.0 || target.value > 100.0) {
                return usageError("zoom: " + args[0] + " is outside the supported range 0-100%");
            }
        } else if (target.value < kMin || target.value > kMax) {
            return usageError("zoom: " + args[0] + " is outside the supported range 1.0-2.0");
        }
    }

    // V4L2 first: see v4l2() for why. Only a camera with no OBSBOT V4L2 node
    // falls through to the SDK.
    if (V4l2Backend *v = v4l2()) {
        const V4l2Backend::ControlRange range = v->getZoomRange();
        if (!range.valid) {
            cerr << "obsbot-cli: zoom: " << v->devicePath()
                 << " does not expose zoom_absolute" << endl;
            return ExitDeviceError;
        }
        const double span = range.max - range.min;

        // A ratio spans the range; a percentage addresses it directly.
        const auto toRaw = [&](const ZoomArg &a) {
            return a.percent ? range.min + (a.value / 100.0) * span
                             : range.min + ((a.value - kMin) / (kMax - kMin)) * span;
        };

        double raw;
        if (relative) {
            // Read the device rather than a stored value: zoom_absolute
            // reports the real current setting.
            const int current = v->getZoomAbsolute();
            if (current < 0) {
                cerr << "obsbot-cli: zoom: could not read the current zoom level" << endl;
                return ExitDeviceError;
            }
            const double delta = (step.percent ? step.value / 100.0
                                               : step.value / (kMax - kMin)) * span;
            raw = current + (args[0] == "in" ? delta : -delta);
        } else {
            raw = toRaw(target);
        }

        const int value = snapToRange(raw, range);
        const double pct = span > 0 ? (value - range.min) * 100.0 / span : 0.0;
        char label[64];
        snprintf(label, sizeof(label), "zoom %d (%.0f%% of range)", value, pct);
        return v4l2Result(label, v->setZoomAbsolute(value));
    }

    auto dev = device();
    if (!dev) return ExitNoDevice;

    double ratio = target.percent ? kMin + (target.value / 100.0) * (kMax - kMin)
                                  : target.value;
    if (relative) {
        // Zoom, unlike pan/tilt, is reported by the camera, so relative moves
        // need no stored state.
        Device::CameraStatus st;
        if (!readStatus(dev, st)) return ExitDeviceError;
        const double current = st.tiny.zoom_ratio / 100.0;
        const double delta = step.percent ? (step.value / 100.0) * (kMax - kMin) : step.value;
        ratio = clampRange(args[0] == "in" ? current + delta : current - delta, kMin, kMax);
    }

    char label[32];
    snprintf(label, sizeof(label), "zoom %.2fx", ratio);
    return sdkResult(label, dev->cameraSetZoomAbsoluteR(static_cast<float>(ratio)));
}

/// Pan/tilt on the V4L2 path, in the device's own arc-second units.
///
/// Whether `pan_absolute`/`tilt_absolute` report true gimbal position or merely
/// echo the last write is UNRESOLVED -- see finding PROJ-010-F001. Reading the
/// device is still the better source for a relative move than the stored
/// config value: under the echo hypothesis it is exactly equivalent, and if
/// readback is live it is strictly better. The config shadow is kept updated
/// regardless, so nothing that depended on it regresses.
struct PanTiltRanges {
    V4l2Backend::ControlRange pan;
    V4l2Backend::ControlRange tilt;
    bool valid() const { return pan.valid && tilt.valid; }
};

PanTiltRanges panTiltRanges(V4l2Backend *v)
{
    return {v->getPanRange(), v->getTiltRange()};
}

/// Arc-seconds per degree, the unit V4L2 uses for pan/tilt. The Tiny 3's step
/// is 3600, i.e. exactly one degree of *commanded* angle.
///
/// Commanded degrees are not motor degrees. Measured on a Tiny 3 against
/// gimbalGetAttitudeInfoR: commanding -40 reached 37.17 deg of motor pitch,
/// -70 reached 67.71, -74 reached 73.76. The camera undershoots by a couple of
/// degrees in the mid range and the error is not a constant scale factor, so
/// no correction is applied here -- applying one from four samples on a single
/// axis in a single direction would invent precision we do not have. Treat a
/// commanded angle as accurate to within a few degrees. See PROJ-010-F004.
const double kArcSecPerDegree = 3600.0;

/// Safe commanded-angle limits, in degrees, tighter than anything the device
/// advertises.
///
/// Both nominal sources over-report tilt travel: V4L2 VIDIOC_QUERYCTRL reports
/// +/-324000 (+/-90 deg) and the SDK's aiGetGimbalParaR reports pitch
/// +/-90 deg, but the real mechanical stop is at about 76.2 deg of motor
/// pitch. Commanding past it does not simply saturate harmlessly: once the
/// gimbal is driven into the stop it stops responding to further commands
/// altogether -- a sweep that ended by commanding 0 deg left the motor sitting
/// at +76.21 -- and only gimbalRstPosR() recovers it. So the limit here is not
/// a cosmetic clamp, it is what keeps a user from wedging their camera.
///
/// -70 deg was measured reaching 67.71 deg of motor pitch, leaving roughly
/// 8 deg of headroom before the stop. Pan is left at its queried range: both
/// extremes (+/-130 deg) were confirmed to move without wedging.
///
/// This bound is symmetric but the evidence behind it is not: only the
/// DOWNWARD stop was measured against motor angle. Commanding +83 deg up did
/// move the camera, so upward travel is very likely greater than 70 deg and
/// this limit needlessly restricts it. Symmetric is the safe direction to err
/// while the upward stop is unmeasured -- an over-tight limit costs a user
/// some range, an over-loose one wedges their gimbal. Measure the upward stop
/// with gimbalGetAttitudeInfoR and split this into two constants.
const double kTiltSafeDegrees = 70.0;

int applyPanTiltRaw(V4l2Backend *v, const string &label, int panRaw, int tiltRaw)
{
    if (!v->setPanAbsolute(panRaw)) return v4l2Result(label, false);
    return v4l2Result(label, v->setTiltAbsolute(tiltRaw));
}

/// `orient <pan-deg> <tilt-deg>` points the camera at an absolute position in
/// degrees, which is what a start-of-day "look here" needs. Degrees rather than
/// the device's arc-seconds because a caller should not have to multiply by
/// 3600, and rather than pan-tilt's normalized -1.0..1.0 because a physical
/// angle is what makes an orientation reproducible across models with
/// different travel.
/// Reject a commanded angle we cannot honour, rather than clamping it.
///
/// Clamping is the wrong default for an orientation command: a caller asking
/// to point at an angle the hardware cannot reach wants to be told, not
/// quietly aimed somewhere else. Readback cannot be used to detect the
/// substitution afterwards either, since it echoes the last accepted command
/// regardless of where the gimbal actually went.
bool checkAxis(const char *axis, double deg, double limit, const string &cmd)
{
    if (deg >= -limit && deg <= limit) return true;
    cerr << "obsbot-cli: " << cmd << ": " << axis << " " << deg
         << " deg is outside the usable range +/-" << limit << " deg" << endl;
    return false;
}

int cmdOrient(const DeviceProvider &, Config &config, const vector<string> &args)
{
    if (args.size() != 2) {
        return usageError("orient: expected '<pan-degrees> <tilt-degrees>'");
    }
    double panDeg = 0.0, tiltDeg = 0.0;
    if (!parseDouble(args[0], panDeg) || !parseDouble(args[1], tiltDeg)) {
        return usageError("orient: pan and tilt must be numbers in degrees");
    }

    // The tilt bound is a fixed measured constant, so check it before the
    // device is touched: an unreachable angle should fail instantly rather
    // than after a device scan, the same way a bad argument does.
    if (!checkAxis("tilt", tiltDeg, kTiltSafeDegrees, "orient")) return ExitUsage;

    V4l2Backend *v = v4l2();
    if (!v) {
        cerr << "obsbot-cli: orient: no OBSBOT V4L2 device found;"
             << " orient is V4L2-only" << endl;
        return ExitNoDevice;
    }
    const PanTiltRanges r = panTiltRanges(v);
    if (!r.valid()) {
        cerr << "obsbot-cli: orient: " << v->devicePath()
             << " does not expose pan_absolute and tilt_absolute" << endl;
        return ExitDeviceError;
    }

    // Pan's bound comes from the driver's queried range, which both extremes
    // were confirmed to reach without wedging, so it needs the device.
    const double panLimitDeg = r.pan.max / kArcSecPerDegree;
    if (!checkAxis("pan", panDeg, panLimitDeg, "orient")) return ExitUsage;

    const int panRaw  = snapToRange(panDeg * kArcSecPerDegree, r.pan);
    const int tiltRaw = snapToRange(tiltDeg * kArcSecPerDegree, r.tilt);

    char label[96];
    snprintf(label, sizeof(label), "orient %.0f %.0f (pan %d, tilt %d)",
             panRaw / kArcSecPerDegree, tiltRaw / kArcSecPerDegree, panRaw, tiltRaw);
    const int rc = applyPanTiltRaw(v, label, panRaw, tiltRaw);

    if (rc == ExitOk) {
        // Keep the shadow honest for pan-tilt's relative moves.
        Config::CameraSettings settings = config.getSettings();
        settings.pan  = r.pan.max  ? static_cast<double>(panRaw)  / r.pan.max  : 0.0;
        settings.tilt = r.tilt.max ? static_cast<double>(tiltRaw) / r.tilt.max : 0.0;
        config.setSettings(settings);
        if (config.isSavingEnabled()) config.save();
    }
    return rc;
}

/// Re-home the gimbal through the SDK.
///
/// This is a recovery command, not a convenience. Driving the tilt axis into
/// its mechanical stop leaves the gimbal ignoring every subsequent V4L2
/// position write, and nothing on the V4L2 path gets it back -- so without
/// this a user who overshoots has no way to recover from the CLI. Measured on
/// a Tiny 3: gimbalRstPosR() restored a gimbal wedged at +76.21 deg of motor
/// pitch to level, and separately recovered one sitting at -90.50.
///
/// Deliberately NOT implemented with cameraSetPanTiltAbsolute(0,0): that call
/// produced a clean level home on several attempts but on another drove pitch
/// to -90.50, making a wedged gimbal worse. gimbalRstPosR has not failed.
///
/// This pays the SDK's device-detection cost, unlike the V4L2-backed commands.
/// That is the right trade for a command a user reaches for when the camera is
/// already pointing at the floor.
int cmdRecenter(const DeviceProvider &device, Config &config, const vector<string> &args)
{
    if (!args.empty()) return usageError("recenter: takes no arguments");

    auto dev = device();
    if (!dev) return ExitNoDevice;

    const int rc = sdkResult("recenter", dev->gimbalRstPosR());
    if (rc == ExitOk) {
        // The gimbal is now at its own home, so the shadow's old position is
        // stale in a way that would send the next relative move somewhere odd.
        Config::CameraSettings settings = config.getSettings();
        settings.pan = 0.0;
        settings.tilt = 0.0;
        config.setSettings(settings);
        if (config.isSavingEnabled()) config.save();
    }
    return rc;
}

/// Pan/tilt readback echoes the last accepted command rather than reporting
/// position (see Commands.h), so relative moves still need the stored position
/// in the config file for continuity across the one-shot invocations a Stream
/// Deck makes. Absolute moves persist too, to keep the stored value honest.
/// The device is read in preference to the shadow where possible, which under
/// the echo behaviour is equivalent but also picks up writes made by other
/// tools.
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

    // V4L2 first, for the same latency reason as zoom: see v4l2().
    V4l2Backend *v = v4l2();
    shared_ptr<Device> dev;
    if (!v) {
        dev = device();
        if (!dev) return ExitNoDevice;
    }

    Config::CameraSettings settings = config.getSettings();
    double pan = settings.pan;
    double tilt = settings.tilt;

    PanTiltRanges ranges{};
    if (v) {
        ranges = panTiltRanges(v);
        if (!ranges.valid()) {
            cerr << "obsbot-cli: pan-tilt: " << v->devicePath()
                 << " does not expose pan_absolute and tilt_absolute" << endl;
            return ExitDeviceError;
        }
        // Prefer the device's own value over the stored shadow for relative
        // moves; see the note above PanTiltRanges on why this is safe even
        // though readback faithfulness is unresolved.
        const int curPan = v->getPanAbsolute();
        const int curTilt = v->getTiltAbsolute();
        if (curPan >= ranges.pan.min && ranges.pan.max != 0) {
            pan = static_cast<double>(curPan) / ranges.pan.max;
        }
        if (curTilt >= ranges.tilt.min && ranges.tilt.max != 0) {
            tilt = static_cast<double>(curTilt) / ranges.tilt.max;
        }
    }

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

    if (v) {
        // Same mechanical-stop guard as orient: pan-tilt's -1.0..1.0 maps
        // onto the queried range, whose tilt extreme would wedge the gimbal.
        const double tiltLimit = kTiltSafeDegrees * kArcSecPerDegree;
        const int panRaw = snapToRange(pan * ranges.pan.max, ranges.pan);
        double tiltReq = tilt * ranges.tilt.max;
        if (tiltReq >  tiltLimit) tiltReq =  tiltLimit;
        if (tiltReq < -tiltLimit) tiltReq = -tiltLimit;
        const int tiltRaw = snapToRange(tiltReq, ranges.tilt);
        const int rc = applyPanTiltRaw(v, label, panRaw, tiltRaw);
        if (rc != ExitOk) return rc;
    } else {
        int32_t ret = dev->cameraSetPanTiltAbsolute(pan, tilt);
        if (ret != 0) return sdkResult(label, ret);
    }

    settings.pan = pan;
    settings.tilt = tilt;
    config.setSettings(settings);
    if (config.isSavingEnabled() && !config.save()) {
        // The camera did move, so this is a warning, not a failure: only the
        // next relative move is affected.
        cerr << "obsbot-cli: warning: could not save position to " << config.getConfigPath()
             << "; the next relative move will start from the stored value" << endl;
    }
    return v ? v4l2Result(label, true) : sdkResult(label, 0);
}

/// `whitebal auto|<kelvin>` -- and with no argument, report the current
/// setting.
///
/// The Kelvin figure is the colour temperature of the light in the room.
/// Telling the camera the light is warm (a low number) makes it compensate by adding blue.
/// A stale 2000K on this camera produced an unusably blue image for exactly
/// that reason.
///
/// Uses V4L2 rather than the SDK for the same latency reason as zoom, and it is
/// the only path that offers auto: the SDK's DevWhiteBalanceAuto is a
/// mode value, whereas V4L2 exposes auto as its own control that the camera
/// then drives continuously.
int cmdWhiteBalance(const DeviceProvider &, Config &config, const vector<string> &args)
{
    if (args.size() > 1) {
        return usageError("whitebal: expected 'auto'|<kelvin>, or no argument to report");
    }

    // Parse before touching the device, as elsewhere: a typo should not cost a
    // device probe.
    bool wantAuto = false;
    int kelvin = 0;
    const bool reporting = args.empty();
    if (!reporting) {
        if (args[0] == "auto") {
            wantAuto = true;
        } else if (!parseInt(args[0], kelvin)) {
            return usageError("whitebal: expected 'auto' or a temperature in Kelvin, got '" +
                              args[0] + "'");
        }
    }

    V4l2Backend *v = v4l2();
    if (!v) {
        cerr << "obsbot-cli: whitebal: no OBSBOT V4L2 device found" << endl;
        return ExitNoDevice;
    }

    if (reporting) {
        const bool isAuto = v->getWhiteBalanceAuto();
        out() << "whitebal " << (isAuto ? "auto" : "manual");
        // The temperature control reads as inactive under auto, so its value
        // is only meaningful when auto is off.
        if (!isAuto) out() << " " << v->getWhiteBalanceTemperature() << "K";
        out() << "\n";
        return ExitOk;
    }

    // Persist whichever mode is chosen. Saving only the manual path would
    // leave `apply` pushing a stale mode: the config file is what a bare
    // `obsbot-cli` replays, so an unsaved `whitebal auto` would be silently
    // undone the next time the config is applied.
    const auto persist = [&config](int mode, int kelvin) {
        Config::CameraSettings settings = config.getSettings();
        settings.whiteBalance = mode;
        if (kelvin > 0) settings.whiteBalanceKelvin = kelvin;
        config.setSettings(settings);
        if (config.isSavingEnabled() && !config.save()) {
            cerr << "obsbot-cli: warning: could not save white balance to "
                 << config.getConfigPath() << endl;
        }
    };

    if (wantAuto) {
        const int rc = v4l2Result("whitebal auto", v->setWhiteBalanceAuto(true));
        // 0 is the SDK's DevWhiteBalanceAuto, which is what Config stores.
        if (rc == ExitOk) persist(0, 0);
        return rc;
    }

    const V4l2Backend::ControlRange range = v->getWhiteBalanceTemperatureRange();
    if (!range.valid) {
        cerr << "obsbot-cli: whitebal: " << v->devicePath()
             << " does not expose white_balance_temperature" << endl;
        return ExitDeviceError;
    }
    if (kelvin < range.min || kelvin > range.max) {
        cerr << "obsbot-cli: whitebal: " << kelvin << "K is outside the supported range "
             << range.min << "-" << range.max << "K" << endl;
        return ExitUsage;
    }

    // Manual temperature is ignored while auto is engaged, so auto has to come
    // off first or the write would silently do nothing.
    if (!v->setWhiteBalanceAuto(false)) {
        return v4l2Result("whitebal: disabling auto", false);
    }
    const int snapped = snapToRange(kelvin, range);
    char label[64];
    snprintf(label, sizeof(label), "whitebal %dK", snapped);
    const int rc = v4l2Result(label, v->setWhiteBalanceTemperature(snapped));

    // Config models manual mode as the SDK's DevWhiteBalanceManual (255)
    // plus a Kelvin value.
    if (rc == ExitOk) persist(255, snapped);
    return rc;
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

/// Which section of --help a command appears under. Grouping beats alphabetical
/// here because the commands split cleanly by what they act on: where the
/// camera points and how it gathers light, versus how it processes the image,
/// versus the two that are neither.
enum Group {
    GroupView,   /// aiming and optics -- moves the gimbal or changes the lens
    GroupImage,  /// AI and colour processing applied to the captured image
    GroupOther,  /// stored slots and state reporting
};

struct Command {
    const char *name;
    const char *args;
    const char *summary;
    int (*handler)(const DeviceProvider &, Config &, const vector<string> &);
    Group group;
};

struct GroupInfo {
    Group group;
    const char *title;
};

const GroupInfo kGroups[] = {
    {GroupView,  "Framing and optics"},
    {GroupImage, "Image tuning (AI and colour)"},
    {GroupOther, "Presets and state"},
};

const Command kCommands[] = {
    // Framing and optics: these change where the camera looks or how the lens
    // gathers light, under direct instruction. The AI-driven equivalents that
    // decide for themselves where to look are grouped as AI instead.
    {"focus",      "auto|<0-100>|in|out [step]",    "focus",                                cmdFocus,       GroupView},
    {"fov",        "wide|medium|narrow|cycle",      "field of view (86/78/65 degrees)",     cmdFov,         GroupView},
    {"orient",     "<pan-deg> <tilt-deg>",          "absolute gimbal angle in degrees",     cmdOrient,      GroupView},
    {"pan-tilt",   "<pan> <tilt>|center|<dir>",     "gimbal; dir = left right up down",     cmdPanTilt,     GroupView},
    {"recenter",   "",                              "re-home the gimbal (SDK; recovery)",   cmdRecenter,    GroupView},
    {"zoom",       "<1.0-2.0>|<n>%|in|out|reset",   "zoom; 1.0-2.0 spans the full range",   cmdZoom,        GroupView},

    // AI and colour. Note this group is not purely image processing: `ai` and
    // `tracking` move the gimbal as a side effect of what the AI decides to
    // follow, so they sit here with the other AI features rather than with the
    // commands that aim the camera on explicit instruction.
    {"ai",         "<mode>|toggle <mode>",          "none group single hand whiteboard desk", cmdAi,        GroupImage},
    {"face-ae",    "on|off|toggle",                 "face auto-exposure",                   cmdFaceAe,      GroupImage},
    {"face-focus", "on|off|toggle",                 "face auto-focus",                      cmdFaceFocus,   GroupImage},
    {"gesture",    "on|off|toggle [target|zoom|all]","hand-gesture triggers (default target)",cmdGesture,    GroupImage},
    {"hdr",        "on|off|toggle",                 "HDR (wide dynamic range)",             cmdHdr,         GroupImage},
    {"tracking",   "on|off",                        "auto-framing (media mode)",            cmdTracking,    GroupImage},
    {"whitebal",   "auto|<kelvin>",                 "room light colour temp (2000-10000K)", cmdWhiteBalance,GroupImage},

    // Neither of the above.
    {"preset",     "[save] 1|2|3",                  "recall or store a pan/tilt/zoom slot", cmdPreset,      GroupOther},
    {"status",     "",                              "print current state as key=value",     cmdStatus,      GroupOther},
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
    for (const auto &g : kGroups) {
        cout << "\n" << g.title << ":\n";
        for (const auto &c : kCommands) {
            if (c.group != g.group) continue;
            const string invocation = string(c.name) + " " + c.args;
            cout << "  " << invocation;
            // Pad to the column, but never run the two together when an
            // invocation is longer than the column is wide.
            size_t pad = invocation.size() < 34 ? 34 - invocation.size() : 2;
            cout << string(pad, ' ') << c.summary << "\n";
        }
    }
    cout << "\nExamples:\n"
         << "  obsbot-cli hdr toggle\n"
         << "  obsbot-cli ai toggle single\n"
         << "  obsbot-cli zoom in 0.25\n"
         << "  obsbot-cli zoom 40%\n"
         << "  obsbot-cli preset 2\n";
}

}  // namespace cli
