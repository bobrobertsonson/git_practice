#pragma once

#include <filesystem>

#include "sawblade/wav_io.h"

namespace sawblade::plugin {

// Decodes a song file for separation (message / separation thread, never the audio thread): wav, flac and
// mp3 with the core decoders; anything else (m4a / aac through CoreAudio on macOS, aiff, ogg) with JUCE's
// AudioFormatManager. Throws std::runtime_error (path in the message) when no decoder can read the file.
AudioFile decodeSongFile(const std::filesystem::path& path);

}  // namespace sawblade::plugin
