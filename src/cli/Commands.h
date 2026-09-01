#ifndef CLI_COMMANDS_H
#define CLI_COMMANDS_H

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

class Config;
class Device;

namespace cli {

/// Process exit status. Stream Deck plugins and shell scripts branch on these,
/// so they are part of the CLI's contract: only ExitOk means the camera was
/// actually changed.
enum ExitCode {
    ExitOk = 0,
    ExitDeviceError = 1,  /// the command was understood but the SDK refused it
    ExitUsage = 2,        /// unknown command or bad arguments; nothing was sent
    ExitNoDevice = 3,     /// no camera was found before the detection timeout
};

/// Resolves the camera, returning nullptr if none appeared. Commands call this
/// only after their arguments validate, so a typo costs no device scan.
using DeviceProvider = std::function<std::shared_ptr<Device>()>;

/// Command output. The SDK logs unconditionally to stdout, so one-shot mode
/// points fd 1 at stderr and buffers real output here instead; writeOutput()
/// then emits it to the caller's actual stdout. Without this, `status` output
/// would be interleaved with SDK chatter and unparseable.
std::ostream &out();

/// Write everything accumulated in out() to @p fd (the saved real stdout).
void writeOutput(int fd);

/// True if @p name is a one-shot command, i.e. argv[1] should be dispatched
/// through run() rather than treated as a flag.
bool isCommand(const std::string &name);

/// Execute one command. @p config is the already-loaded configuration;
/// commands needing state the camera cannot report back (pan/tilt) read and
/// write it through @p config.
int run(const DeviceProvider &device, Config &config,
        const std::string &name, const std::vector<std::string> &args);

/// Print the command reference for --help.
void printUsage();

}  // namespace cli

#endif  // CLI_COMMANDS_H
