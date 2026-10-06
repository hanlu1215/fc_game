#pragma once

#include <string>

namespace nes {

// The handful of frontend preferences that should outlive a run. Right now
// that is only the volume, kept as "key = value" lines so adding another one
// later does not need a format change.
//
//   $XDG_DATA_HOME/fc_game/settings.conf
//   ~/.local/share/fc_game/settings.conf
//
// The file sits next to the save states for the same reason they do: it is
// per-user state, not something that belongs beside the executable.
class Settings {
public:
    // Reads the file. A missing or unreadable file is not an error -- the
    // emulator just starts on its defaults. Returns true when a volume was
    // found, so the caller can tell "never set" from "set to the default".
    bool load();

    // Writes the current values back. Best-effort: a failure here is silently
    // ignored, since losing a preference is not worth interrupting play for.
    void save() const;

    static constexpr int kDefaultVolume = 100;

    int  volume() const { return volume_; }
    bool hasVolume() const { return hasVolume_; }
    void setVolume(int percent);

    static std::string path();

private:
    int  volume_ = kDefaultVolume;
    bool hasVolume_ = false;
};

} // namespace nes
