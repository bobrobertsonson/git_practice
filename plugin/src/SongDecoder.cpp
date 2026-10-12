#include "SongDecoder.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

#include <juce_audio_formats/juce_audio_formats.h>

namespace sawblade::plugin {

AudioFile decodeSongFile(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext == ".wav" || ext == ".flac" || ext == ".mp3") return readAudioFile(path);

  juce::AudioFormatManager fm;
  fm.registerBasicFormats();  // CoreAudio (m4a, aac, mp3) on macOS, AIFF, OGG
  std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(juce::File(path.string())));
  if (!reader || reader->numChannels == 0 || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    throw std::runtime_error("cannot decode " + path.string() + " on this system (supported: mp3, wav, flac; m4a needs macOS)");
  const int ch = static_cast<int>(std::min<unsigned>(reader->numChannels, 2u));
  const juce::int64 total = reader->lengthInSamples;
  AudioFile out;
  out.sampleRate = reader->sampleRate;
  out.channels = ch;
  out.interleaved.resize(static_cast<std::size_t>(total) * static_cast<std::size_t>(ch));
  constexpr int kChunk = 1 << 16;
  juce::AudioBuffer<float> buf(ch, kChunk);
  for (juce::int64 pos = 0; pos < total; pos += kChunk) {
    const int n = static_cast<int>(std::min<juce::int64>(kChunk, total - pos));
    if (!reader->read(&buf, 0, n, pos, true, ch > 1))
      throw std::runtime_error("read error while decoding " + path.string());
    for (int c = 0; c < ch; ++c) {
      const float* src = buf.getReadPointer(c);
      for (int i = 0; i < n; ++i)
        out.interleaved[(static_cast<std::size_t>(pos) + static_cast<std::size_t>(i)) * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = src[i];
    }
  }
  return out;
}

}  // namespace sawblade::plugin
