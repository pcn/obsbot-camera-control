#include "V4l2Backend.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <linux/videodev2.h>

V4l2Backend::V4l2Backend() = default;

V4l2Backend::~V4l2Backend()
{
    close();
}

bool V4l2Backend::open(const std::string &devicePath)
{
    close();
    int fd = ::open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return false;
    ::close(fd);
    m_devicePath = devicePath;
    m_configured = true;
    return true;
}

void V4l2Backend::close()
{
    m_configured = false;
    m_devicePath.clear();
}

int V4l2Backend::openDevice() const
{
    if (m_devicePath.empty())
        return -1;
    return ::open(m_devicePath.c_str(), O_RDWR | O_NONBLOCK);
}

/// True if @p fd exposes @p id. The Tiny 3 presents two nodes with the same
/// card name ("OBSBOT Tiny 3: OBSBOT Tiny 3 St"), and only one of them takes
/// camera controls; the other is metadata-only. Probing a control is what
/// separates them, since the names are identical and node numbering is not
/// stable across re-plugs.
static bool hasControl(int fd, uint32_t id)
{
    struct v4l2_queryctrl qctrl{};
    qctrl.id = id;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) != 0)
        return false;
    return (qctrl.flags & V4L2_CTRL_FLAG_DISABLED) == 0;
}

std::string V4l2Backend::findObsbotDevice()
{
    // An explicit override wins outright: auto-detection cannot be right for
    // every host, and a user with two OBSBOT cameras needs a way to say which.
    if (const char *forced = getenv("OBSBOT_VIDEO_DEVICE")) {
        if (*forced)
            return forced;
    }

    DIR *dir = opendir("/dev");
    if (!dir)
        return {};

    // Collect and sort rather than returning the first hit: readdir order is
    // arbitrary, so picking as we go would choose a different node run to run.
    std::vector<std::string> candidates;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("video") != 0)
            continue;

        std::string path = "/dev/" + name;
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        struct v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            std::string card(reinterpret_cast<const char *>(cap.card));
            const bool isVideoCapture = (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
            // Require a real control, not just the OBSBOT name: on the Tiny 3
            // the metadata node carries the same card string and would
            // otherwise be accepted, leaving every ioctl to fail later.
            if (card.find("OBSBOT") != std::string::npos && isVideoCapture &&
                hasControl(fd, V4L2_CID_ZOOM_ABSOLUTE)) {
                candidates.push_back(path);
            }
        }
        ::close(fd);
    }
    closedir(dir);

    if (candidates.empty())
        return {};
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

std::string V4l2Backend::cardName() const
{
    int fd = openDevice();
    if (fd < 0)
        return {};
    struct v4l2_capability cap{};
    std::string name;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        name = reinterpret_cast<const char *>(cap.card);
    ::close(fd);
    return name;
}

int V4l2Backend::getControl(uint32_t id)
{
    int fd = openDevice();
    if (fd < 0)
        return -1;
    struct v4l2_control ctrl{};
    ctrl.id = id;
    int result = -1;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0)
        result = ctrl.value;
    ::close(fd);
    return result;
}

bool V4l2Backend::setControl(uint32_t id, int value)
{
    int fd = openDevice();
    if (fd < 0)
        return false;
    struct v4l2_control ctrl{};
    ctrl.id = id;
    ctrl.value = value;
    bool ok = ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0;
    ::close(fd);
    return ok;
}

V4l2Backend::ControlRange V4l2Backend::queryRange(uint32_t id)
{
    ControlRange range{};
    int fd = openDevice();
    if (fd < 0)
        return range;

    struct v4l2_queryctrl qctrl{};
    qctrl.id = id;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
        range.min = qctrl.minimum;
        range.max = qctrl.maximum;
        range.step = qctrl.step == 0 ? 1 : qctrl.step;
        range.defaultValue = qctrl.default_value;
        range.valid = true;
    }
    ::close(fd);
    return range;
}

bool V4l2Backend::setBrightness(int value) { return setControl(V4L2_CID_BRIGHTNESS, value); }
int V4l2Backend::getBrightness() { return getControl(V4L2_CID_BRIGHTNESS); }
V4l2Backend::ControlRange V4l2Backend::getBrightnessRange() { return queryRange(V4L2_CID_BRIGHTNESS); }

bool V4l2Backend::setContrast(int value) { return setControl(V4L2_CID_CONTRAST, value); }
int V4l2Backend::getContrast() { return getControl(V4L2_CID_CONTRAST); }
V4l2Backend::ControlRange V4l2Backend::getContrastRange() { return queryRange(V4L2_CID_CONTRAST); }

bool V4l2Backend::setSaturation(int value) { return setControl(V4L2_CID_SATURATION, value); }
int V4l2Backend::getSaturation() { return getControl(V4L2_CID_SATURATION); }
V4l2Backend::ControlRange V4l2Backend::getSaturationRange() { return queryRange(V4L2_CID_SATURATION); }

bool V4l2Backend::setWhiteBalanceAuto(bool enabled) { return setControl(V4L2_CID_AUTO_WHITE_BALANCE, enabled ? 1 : 0); }
bool V4l2Backend::getWhiteBalanceAuto() { return getControl(V4L2_CID_AUTO_WHITE_BALANCE) == 1; }
bool V4l2Backend::setWhiteBalanceTemperature(int kelvin) { return setControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, kelvin); }
int V4l2Backend::getWhiteBalanceTemperature() { return getControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE); }
V4l2Backend::ControlRange V4l2Backend::getWhiteBalanceTemperatureRange() { return queryRange(V4L2_CID_WHITE_BALANCE_TEMPERATURE); }

bool V4l2Backend::setPanAbsolute(int value) { return setControl(V4L2_CID_PAN_ABSOLUTE, value); }
int V4l2Backend::getPanAbsolute() { return getControl(V4L2_CID_PAN_ABSOLUTE); }
V4l2Backend::ControlRange V4l2Backend::getPanRange() { return queryRange(V4L2_CID_PAN_ABSOLUTE); }

bool V4l2Backend::setTiltAbsolute(int value) { return setControl(V4L2_CID_TILT_ABSOLUTE, value); }
int V4l2Backend::getTiltAbsolute() { return getControl(V4L2_CID_TILT_ABSOLUTE); }
V4l2Backend::ControlRange V4l2Backend::getTiltRange() { return queryRange(V4L2_CID_TILT_ABSOLUTE); }

bool V4l2Backend::setZoomAbsolute(int value) { return setControl(V4L2_CID_ZOOM_ABSOLUTE, value); }
int V4l2Backend::getZoomAbsolute() { return getControl(V4L2_CID_ZOOM_ABSOLUTE); }
V4l2Backend::ControlRange V4l2Backend::getZoomRange() { return queryRange(V4L2_CID_ZOOM_ABSOLUTE); }

bool V4l2Backend::setAutoExposure(bool automatic)
{
    return setControl(V4L2_CID_EXPOSURE_AUTO, automatic ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL);
}

bool V4l2Backend::setExposureAbsolute(int value) { return setControl(V4L2_CID_EXPOSURE_ABSOLUTE, value); }

bool V4l2Backend::setAutoFocus(bool enabled) { return setControl(V4L2_CID_FOCUS_AUTO, enabled ? 1 : 0); }
bool V4l2Backend::getAutoFocus() { return getControl(V4L2_CID_FOCUS_AUTO) == 1; }
bool V4l2Backend::setFocusAbsolute(int value) { return setControl(V4L2_CID_FOCUS_ABSOLUTE, value); }
int V4l2Backend::getFocusAbsolute() { return getControl(V4L2_CID_FOCUS_ABSOLUTE); }
