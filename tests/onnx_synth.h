#pragma once

// Builds, in C++ at test time, a tiny ONNX model with the I/O of the real htdemucs core graph:
//   inputs   mix (1,2,343980)  mag (1,4,2048,336)
//   outputs  x_freq (1,S,4,2048,336) = a[s] * mag     x_time (1,S,2,343980) = w[s] * mix
// so the separator's whole-song result is analytically known: stem s = (w[s] + a[s]) * mix (per-song
// normalisation is affine, the STFT / iSTFT pair is the identity away from segment edges). The
// protobuf is hand-encoded (no Python, no onnx package). Weights: none; nothing here comes from htdemucs.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sawblade::test::onnx_synth {

namespace detail {
inline void varint(std::string& o, std::uint64_t v) {
  while (v >= 0x80) {
    o.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  o.push_back(static_cast<char>(v));
}
inline void tag(std::string& o, int field, int wire) { varint(o, (static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(wire)); }
inline void fieldVarint(std::string& o, int field, std::uint64_t v) { tag(o, field, 0); varint(o, v); }
inline void fieldBytes(std::string& o, int field, const std::string& b) { tag(o, field, 2); varint(o, b.size()); o += b; }

inline std::string tensorProto(const std::string& name, const std::vector<std::int64_t>& dims, int dataType, const std::string& raw) {
  std::string t;
  for (auto d : dims) fieldVarint(t, 1, static_cast<std::uint64_t>(d));
  fieldVarint(t, 2, static_cast<std::uint64_t>(dataType));
  fieldBytes(t, 8, name);
  fieldBytes(t, 9, raw);
  return t;
}
inline std::string valueInfo(const std::string& name, const std::vector<std::int64_t>& dims) {
  std::string shape;
  for (auto d : dims) {
    std::string dim;
    fieldVarint(dim, 1, static_cast<std::uint64_t>(d));
    fieldBytes(shape, 1, dim);
  }
  std::string tensorType;
  fieldVarint(tensorType, 1, 1);  // FLOAT
  fieldBytes(tensorType, 2, shape);
  std::string type;
  fieldBytes(type, 1, tensorType);
  std::string vi;
  fieldBytes(vi, 1, name);
  fieldBytes(vi, 2, type);
  return vi;
}
inline std::string node(const std::vector<std::string>& in, const std::string& out, const std::string& op) {
  std::string n;
  for (const auto& i : in) fieldBytes(n, 1, i);
  fieldBytes(n, 2, out);
  fieldBytes(n, 4, op);
  return n;
}
inline std::string floats(const std::vector<float>& v) {
  std::string raw(v.size() * sizeof(float), '\0');
  std::memcpy(raw.data(), v.data(), raw.size());
  return raw;
}
}  // namespace detail

// timeGain / freqGain: S entries each (S = 4 or 6).
inline std::string buildCoreModel(const std::vector<float>& timeGain, const std::vector<float>& freqGain) {
  using namespace detail;
  const std::int64_t S = static_cast<std::int64_t>(timeGain.size());
  std::string graph;
  fieldBytes(graph, 1, node({"mix", "axes1"}, "mix_u", "Unsqueeze"));
  fieldBytes(graph, 1, node({"mix_u", "w"}, "x_time", "Mul"));
  fieldBytes(graph, 1, node({"mag", "axes1"}, "mag_u", "Unsqueeze"));
  fieldBytes(graph, 1, node({"mag_u", "a"}, "x_freq", "Mul"));
  fieldBytes(graph, 2, "sawblade_synthetic_core");
  const std::int64_t one = 1;
  fieldBytes(graph, 5, tensorProto("axes1", {1}, 7, std::string(reinterpret_cast<const char*>(&one), sizeof one)));
  fieldBytes(graph, 5, tensorProto("w", {1, S, 1, 1}, 1, floats(timeGain)));
  fieldBytes(graph, 5, tensorProto("a", {1, S, 1, 1, 1}, 1, floats(freqGain)));
  fieldBytes(graph, 11, valueInfo("mix", {1, 2, 343980}));
  fieldBytes(graph, 11, valueInfo("mag", {1, 4, 2048, 336}));
  fieldBytes(graph, 12, valueInfo("x_freq", {1, S, 4, 2048, 336}));
  fieldBytes(graph, 12, valueInfo("x_time", {1, S, 2, 343980}));
  std::string opset;
  fieldVarint(opset, 2, 17);
  std::string model;
  fieldVarint(model, 1, 8);  // ir_version
  fieldBytes(model, 2, "sawblade-test");
  fieldBytes(model, 7, graph);
  fieldBytes(model, 8, opset);
  return model;
}

inline void write(const std::filesystem::path& p, const std::string& bytes) {
  std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace sawblade::test::onnx_synth
