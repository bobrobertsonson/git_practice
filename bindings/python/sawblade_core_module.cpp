// sawblade_core: Python bindings for the offline renderer (spec: docs/specs/phase3_matcher.md 3.1).
//
//   render(preset, audio, sample_rate, render_rate="auto", out_rate="input", block=256,
//          base_dir=None, cache=None) -> (float32 ndarray, report dict)
//   level_match(preset, sample_rate, base_dir=None, cache=None) -> dict (phase 10.1 probe, no audio rendered)
//   CaptureCache: loads each .nam / IR once, shared by any number of renders and threads.
//   load_stems(dir, sample_rate, other_role="guitar") / stem_set_from_arrays(dict, sample_rate) -> StemSet, and
//   StemPlayer (play-along backing, spec docs/specs/phase5_1_stemplayer.md section 5).
//
// The GIL is released for the whole parse + render; nothing in there touches Python objects.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sawblade/capture_cache.h"
#include "sawblade/gate.h"
#include "sawblade/loudness.h"
#include "sawblade/render.h"
#include "sawblade/stem_player.h"
#include "sawblade/stem_set.h"

namespace py = pybind11;
using namespace sawblade;

namespace {

PyObject* gPresetError = nullptr;  // ValueError subclass: the preset is invalid
PyObject* gIoError = nullptr;      // OSError subclass: a file/capture could not be read or verified

void raiseWithPath(PyObject* type, const std::string& message, const std::string& jsonPath) {
  py::object exc = py::reinterpret_borrow<py::object>(type)(message);
  exc.attr("json_path") = jsonPath.empty() ? py::none() : py::object(py::str(jsonPath));
  PyErr_SetObject(type, exc.ptr());
}

// "auto" or a rate in Hz -> optional<double>.
std::optional<double> parseRenderRate(const py::object& o) {
  if (o.is_none()) return std::nullopt;
  if (py::isinstance<py::str>(o)) {
    if (o.cast<std::string>() == "auto") return std::nullopt;
    throw py::value_error("render_rate must be \"auto\" or a rate in Hz");
  }
  const double hz = o.cast<double>();
  if (!(hz >= 1000.0 && hz <= 768000.0)) throw py::value_error("render_rate must be in 1000..768000 Hz");
  return hz;
}

nlohmann::json presetToJson(const py::object& preset) {
  std::string text;
  if (py::isinstance<py::str>(preset)) {
    text = preset.cast<std::string>();
  } else if (py::isinstance<py::dict>(preset)) {
    text = py::module_::import("json").attr("dumps")(preset).cast<std::string>();
  } else {
    throw py::type_error("preset must be a JSON string or a dict");
  }
  nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) throw PresetError("", "invalid JSON");
  return j;
}

py::tuple render(const py::object& preset, const py::array& audioIn, const py::object& sampleRateArg,
                 const py::object& renderRate, const std::string& outRate, const py::object& blockArg,
                 const py::object& baseDir, CaptureCache* cache, const py::object& deviceDbu, const std::string& diChannel,
                 const std::string& calibrationArg) {
  if (py::isinstance<py::bool_>(sampleRateArg) || py::isinstance<py::str>(sampleRateArg) ||
      !(PyNumber_Check(sampleRateArg.ptr())))
    throw py::value_error("sample_rate must be a number (Hz)");
  const double sampleRate = sampleRateArg.cast<double>();
  if (!(std::isfinite(sampleRate) && sampleRate >= 1000.0)) throw py::value_error("sample_rate must be a rate in Hz, >= 1000");
  if (py::isinstance<py::bool_>(blockArg) || !PyIndex_Check(blockArg.ptr()))
    throw py::value_error("block must be an integer in 1..65536");
  const auto block = py::cast<long long>(py::reinterpret_steal<py::object>(PyNumber_Index(blockArg.ptr())));
  if (block < 1 || block > 65536) throw py::value_error("block must be in 1..65536");
  if (audioIn.dtype().kind() != 'f')
    throw py::value_error("audio must be a floating-point array (expects float audio in [-1, 1]); convert integer PCM first");
  if (audioIn.ndim() == 2 && audioIn.shape(0) == 1 && audioIn.shape(1) > 1)
    throw py::value_error("audio has shape (1, N): 2-D audio is (frames, channels), so this looks channels-first; pass audio.T or audio[0]");
  const auto audio = py::array_t<float, py::array::c_style | py::array::forcecast>::ensure(audioIn);
  if (!audio) throw py::value_error("audio could not be converted to float32");
  if (audio.ndim() != 1 && audio.ndim() != 2)
    throw py::value_error("audio must be 1-D (mono) or 2-D (frames, channels)");
  if (audio.size() == 0) throw py::value_error("audio is empty");
  RenderOptions opts;
  opts.blockSize = static_cast<int>(block);
  opts.renderRate = parseRenderRate(renderRate);
  if (outRate == "input") opts.outRate = OutRate::Input;
  else if (outRate == "render") opts.outRate = OutRate::Render;
  else throw py::value_error("out_rate must be \"input\" or \"render\"");
  opts.cache = cache;
  // v0.8 I4a. calibration: "preset" (default) follows the preset's calibration.mode (v1-v4 files are legacy = off, bit-identical to
  // before), "legacy" forces it off, "calibrated" forces it on. device_dbu: the interface's dBu at 0 dBFS (None = the assumed +12,
  // which the report records). di_channel: which channel of a (frames, channels) array is rendered.
  if (calibrationArg == "preset") opts.calibrationFromPreset = true;
  else if (calibrationArg == "calibrated") opts.calibration.enabled = true;
  else if (calibrationArg != "legacy") throw py::value_error("calibration must be \"preset\", \"legacy\" or \"calibrated\"");
  if (!deviceDbu.is_none()) {
    if (py::isinstance<py::bool_>(deviceDbu) || !PyNumber_Check(deviceDbu.ptr())) throw py::value_error("device_dbu must be a number (dBu at 0 dBFS) or None");
    const double d = deviceDbu.cast<double>();
    if (!(std::isfinite(d) && d >= calibration::kMinPlausibleDbu && d <= calibration::kMaxPlausibleDbu))
      throw py::value_error("device_dbu must be in -60..60 (dBu at 0 dBFS)");
    opts.deviceDbu = d;
  }
  if (diChannel == "auto") opts.diChannel = DiChannel::Auto;
  else if (diChannel == "L") opts.diChannel = DiChannel::Left;
  else if (diChannel == "R") opts.diChannel = DiChannel::Right;
  else if (diChannel == "mix") opts.diChannel = DiChannel::Mix;
  else throw py::value_error("di_channel must be \"auto\", \"L\", \"R\" or \"mix\"");

  const nlohmann::json j = presetToJson(preset);
  std::filesystem::path base = std::filesystem::current_path();
  if (!baseDir.is_none()) base = py::str(py::module_::import("os").attr("fspath")(baseDir)).cast<std::string>();

  AudioFile in;  // copied while we hold the GIL: the array may be mutated by other threads afterwards
  in.sampleRate = sampleRate;
  in.channels = audio.ndim() == 2 ? static_cast<int>(audio.shape(1)) : 1;
  if (in.channels < 1) throw py::value_error("audio has no channels");
  in.interleaved.assign(audio.data(), audio.data() + audio.size());

  RenderResult r;
  std::string report;
  {
    py::gil_scoped_release nogil;
    const Preset p = parsePreset(j, base);
    r = renderPreset(p, in, opts);
    report = reportJson(r).dump();
  }
  py::array_t<float> out(static_cast<py::ssize_t>(r.samples.size()), r.samples.data());
  return py::make_tuple(out, py::module_::import("json").attr("loads")(report));
}

// Phase 10.1: builds the chain, prepare()s it (alignment + level-match probe) and returns the info.
// The probe measures trims whatever the preset's levelMatch.mode, but the trims "in effect" are 0 for
// `off`; the matcher asks for the measurement, so the mode is forced to `auto` here.
py::dict levelMatch(const py::object& preset, const py::object& sampleRateArg, const py::object& baseDir,
                    CaptureCache* cache, const py::object& deviceDbu, const std::string& calibrationArg) {
  if (py::isinstance<py::bool_>(sampleRateArg) || py::isinstance<py::str>(sampleRateArg) ||
      !(PyNumber_Check(sampleRateArg.ptr())))
    throw py::value_error("sample_rate must be a number (Hz)");
  const double sampleRate = sampleRateArg.cast<double>();
  if (!(std::isfinite(sampleRate) && sampleRate >= 1000.0)) throw py::value_error("sample_rate must be a rate in Hz, >= 1000");
  // v0.8 I4a: the probe runs on the same calibrated chain the matcher renders (see render()): "preset" follows the preset's
  // calibration.mode (default), "legacy" forces it off, "calibrated" forces it on at device_dbu (None = the assumed +12 dBu).
  if (calibrationArg != "preset" && calibrationArg != "legacy" && calibrationArg != "calibrated")
    throw py::value_error("calibration must be \"preset\", \"legacy\" or \"calibrated\"");
  std::optional<double> dev;
  if (!deviceDbu.is_none()) {
    if (py::isinstance<py::bool_>(deviceDbu) || !PyNumber_Check(deviceDbu.ptr())) throw py::value_error("device_dbu must be a number (dBu at 0 dBFS) or None");
    const double d = deviceDbu.cast<double>();
    if (!(std::isfinite(d) && d >= calibration::kMinPlausibleDbu && d <= calibration::kMaxPlausibleDbu))
      throw py::value_error("device_dbu must be in -60..60 (dBu at 0 dBFS)");
    dev = d;
  }
  const nlohmann::json j = presetToJson(preset);
  std::filesystem::path base = std::filesystem::current_path();
  if (!baseDir.is_none()) base = py::str(py::module_::import("os").attr("fspath")(baseDir)).cast<std::string>();
  ChainInfo info;
  bool calOn = false;
  {
    py::gil_scoped_release nogil;
    Preset p = parsePreset(j, base);
    p.levelMatch.mode = LevelMatchMode::Auto;
    Chain chain(p, loadResources(p, sampleRate, cache));
    ChainCalibration cal;
    cal.enabled = calibrationArg == "calibrated" || (calibrationArg == "preset" && p.calibrationMode == CalibrationMode::Calibrated);
    cal.device.dbu = dev;
    calOn = cal.enabled;
    if (cal.enabled) chain.setCalibration(cal);  // before prepare(): the probes see the calibrated chain
    chain.prepare({sampleRate, 512});
    info = chain.info();
  }
  const auto lufs = [](double v) { return v > LevelMatchResult::kNoLufs ? py::object(py::float_(v)) : py::none(); };
  py::dict d;
  d["trimADb"] = info.trimDb[0];
  d["trimBDb"] = info.trimDb[1];
  d["lufsA"] = lufs(info.lufs[0]);
  d["lufsB"] = lufs(info.lufs[1]);
  d["sumLufs"] = lufs(info.sumLufs);
  d["makeupDb"] = std::vector<double>(info.makeupDb.begin(), info.makeupDb.end());
  d["delaySamplesB"] = info.align.delaySamplesB;
  d["invertB"] = info.align.invertB;
  d["warnings"] = info.warnings;
  d["calibrated"] = calOn;  // whether the probe ran on a calibrated chain
  return d;
}

// Task G: the core's single resolver of a preset's active dynamics set (activeDynamics), for the exporter / notes.
// Returns {"mode": "live" | "record", "gate": {...}, "busComp": {...}} with the schema's gate / busComp objects.
py::dict resolveDynamicsPy(const py::object& preset, const py::object& baseDir) {
  const nlohmann::json j = presetToJson(preset);
  std::filesystem::path base = std::filesystem::current_path();
  if (!baseDir.is_none()) base = py::str(py::module_::import("os").attr("fspath")(baseDir)).cast<std::string>();
  nlohmann::json out;
  {
    py::gil_scoped_release nogil;
    const Preset p = resolveDynamics(parsePreset(j, base));
    out = {{"mode", effectiveDynamicsMode(p) == DynamicsMode::Live ? "live" : "record"},
           {"gate", toJson(p).at("gate")},
           {"busComp", toJson(p).at("busComp")}};
  }
  return py::module_::import("json").attr("loads")(out.dump()).cast<py::dict>();
}

// H.1: the matcher's DI floor on the gate's own peak detector (core peakFloorDb).
py::object peakFloorPy(const py::array_t<float, py::array::c_style | py::array::forcecast>& x, double fs, double keyHpfHz,
                       const py::object& mask) {
  if (x.ndim() != 1) throw py::value_error("x must be a 1-D array");
  if (!(std::isfinite(fs) && fs >= 1000.0)) throw py::value_error("fs must be a rate in Hz, >= 1000");
  if (!(std::isfinite(keyHpfHz) && keyHpfHz >= 0.0)) throw py::value_error("key_hpf_hz must be >= 0");
  const std::vector<float> xs(x.data(), x.data() + x.size());
  std::vector<std::uint8_t> m;
  const std::vector<std::uint8_t>* mp = nullptr;
  if (!mask.is_none()) {
    const auto ma = py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast>::ensure(mask);
    if (!ma || ma.ndim() != 1 || ma.size() != x.size()) throw py::value_error("mask must be a 1-D array as long as x");
    m.assign(ma.data(), ma.data() + ma.size());
    mp = &m;
  }
  double r;
  {
    py::gil_scoped_release nogil;
    r = peakFloorDb(xs, fs, keyHpfHz, mp);
  }
  if (std::isnan(r)) return py::none();
  return py::float_(r);
}

// ---- stems ------------------------------------------------------------------------------------
StemKind parseKind(const std::string& name) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (n == "drums") return StemKind::Drums;
  if (n == "bass") return StemKind::Bass;
  if (n == "vocals") return StemKind::Vocals;
  if (n == "other") return StemKind::Other;
  if (n == "guitar" || n == "guitars") return StemKind::Guitar;
  throw py::value_error("unknown stem '" + name + "' (use drums, bass, vocals, other, guitar)");
}

StemAudio arrayToStem(const std::string& name, const py::array& in) {
  if (in.dtype().kind() != 'f')
    throw py::value_error("stem '" + name + "' must be a floating-point array (convert integer PCM first)");
  const auto a = py::array_t<float, py::array::c_style | py::array::forcecast>::ensure(in);
  if (!a) throw py::value_error("stem '" + name + "' could not be converted to float32");
  StemAudio out;
  if (a.ndim() == 1) {
    out[0].assign(a.data(), a.data() + a.shape(0));
    out[1] = out[0];
  } else if (a.ndim() == 2 && a.shape(0) == 2) {
    const auto n = static_cast<std::size_t>(a.shape(1));
    out[0].assign(a.data(), a.data() + n);
    out[1].assign(a.data() + n, a.data() + 2 * n);
  } else {
    throw py::value_error("stem '" + name + "' must have shape (n,) or (2, n)");
  }
  return out;
}

std::shared_ptr<StemSet> stemSetFromArrays(const py::dict& d, double sampleRate) {
  std::array<std::optional<StemAudio>, kStemKindCount> audio;
  for (const auto& kv : d) {
    const std::string name = py::str(kv.first).cast<std::string>();
    const StemKind k = parseKind(name);
    if (audio[static_cast<std::size_t>(k)]) throw py::value_error("stem '" + name + "' given twice");
    if (!py::isinstance<py::array>(kv.second)) throw py::type_error("stem '" + name + "' must be a numpy array");
    audio[static_cast<std::size_t>(k)] = arrayToStem(name, py::reinterpret_borrow<py::array>(kv.second));
  }
  return std::make_shared<StemSet>(makeStemSet(sampleRate, audio));
}

// Python holds a StemSet by shared object; set_stem_set copies it into a fresh unique_ptr (fine off
// the audio thread). The GIL is never released around the player, so process() and the producer
// calls are serialised with each other, which is what StemPlayer's threading contract needs.
struct PyStemPlayer : StemPlayer {
  bool prepared = false;
  void need() const {
    if (!prepared) throw std::runtime_error("StemPlayer is not prepared: call prepare(sample_rate, max_block, max_rig_latency) first");
  }
};

GuitarMode parseGuitarMode(std::string m) {
  std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (m == "muted" || m == "mute") return GuitarMode::Muted;
  if (m == "ghost") return GuitarMode::Ghost;
  if (m == "full") return GuitarMode::Full;
  throw py::value_error("guitar mode must be muted, ghost or full");
}

TransportMode parseMode(std::string m) {
  std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (m == "free_run" || m == "freerun") return TransportMode::FreeRun;
  if (m == "host_follow" || m == "hostfollow") return TransportMode::HostFollow;
  throw py::value_error("transport mode must be free_run or host_follow");
}

void bindStems(py::module_& m) {
  py::class_<StemSet, std::shared_ptr<StemSet>>(m, "StemSet", "Immutable stereo stems at one sample rate (load_stems / stem_set_from_arrays).")
      .def_property_readonly("sample_rate", [](const StemSet& s) { return s.sampleRate; })
      .def_property_readonly("length", [](const StemSet& s) { return s.length; })
      .def_property_readonly("other_mapped_to_guitar", [](const StemSet& s) { return s.otherMappedToGuitar; })
      .def_property_readonly("present",
                             [](const StemSet& s) {
                               std::vector<std::string> names;
                               for (int k = 0; k < kStemKindCount; ++k)
                                 if (s.present[static_cast<std::size_t>(k)]) names.push_back(stemKindName(static_cast<StemKind>(k)));
                               return names;
                             })
      .def_property_readonly("warnings", [](const StemSet& s) { return s.warnings; })
      .def_property_readonly("backing_loudness_lufs", [](const StemSet& s) { return s.backingLoudnessLufs; },
                             "BS.1770-4 integrated loudness of the non-guitar stems (float or None). Metadata only.")
      .def("__repr__", [](const StemSet& s) {
        return "<StemSet " + std::to_string(s.length) + " frames @ " + std::to_string(s.sampleRate) + " Hz>";
      });

  m.def(
      "load_stems",
      [](const py::object& dir, double sampleRate, const std::string& otherRole) {
        if (otherRole != "guitar" && otherRole != "other") throw py::value_error("other_role must be 'guitar' or 'other'");
        const std::string path = py::str(py::module_::import("os").attr("fspath")(dir)).cast<std::string>();
        std::shared_ptr<StemSet> out;
        {
          py::gil_scoped_release nogil;
          out = std::make_shared<StemSet>(loadStemDirectory(path, sampleRate, otherRole == "other" ? OtherRole::Other : OtherRole::Guitar));
        }
        return out;
      },
      py::arg("dir"), py::arg("sample_rate"), py::arg("other_role") = "guitar",
      "Load the *.wav / *.flac stems of a directory (drums, bass, vocals, other, guitar|guitars; other audio files are\n"
      "summed into 'other' with a warning), resampled to sample_rate. other_role: 'guitar' (default) loads a 4-stem\n"
      "'other' as the guitar stem when the folder has no guitar file (StemSet.other_mapped_to_guitar tells); 'other' keeps\n"
      "it. Raises RuntimeError on failure.");
  m.def(
      "integrated_loudness_lufs",
      [](const py::array& audio, double sampleRate) -> std::optional<double> {
        const StemAudio a = arrayToStem("audio", audio);
        return integratedLoudnessLufs(a[0].data(), a[1].data(), static_cast<std::int64_t>(a[0].size()), sampleRate);
      },
      py::arg("audio"), py::arg("sample_rate"),
      "ITU-R BS.1770-4 integrated loudness (LUFS) of float audio (n,) (mono, both channels) or (2, n); None for silence or < 400 ms.");
  m.def("stem_set_from_arrays", &stemSetFromArrays, py::arg("stems"), py::arg("sample_rate"),
        "Build a StemSet from {name: float ndarray (n,) or (2, n)} already at sample_rate (padded to the longest).");

  py::class_<PyStemPlayer>(m, "StemPlayer", "Play-along backing player (offline use: process(n) renders n frames).")
      .def(py::init<>())
      .def("prepare",
           [](PyStemPlayer& p, double sampleRate, int maxBlock, int maxRigLatency) {
             p.prepare({sampleRate, maxBlock}, maxRigLatency);
             p.prepared = true;
           },
           py::arg("sample_rate"), py::arg("max_block"), py::arg("max_rig_latency") = 0)
      .def("reset", [](PyStemPlayer& p) { p.need(); p.reset(); })
      .def("set_stem_set",
           [](PyStemPlayer& p, const StemSet& s) {
             p.need();
             p.setStemSet(std::make_unique<StemSet>(s));
             p.collectGarbage();
           },
           py::arg("stem_set"), "Publish a copy of the set; it is adopted at the next process() while the transport is stopped.")
      .def("process",
           [](PyStemPlayer& p, int n) {
             p.need();
             if (n < 0) throw py::value_error("n must be >= 0");
             py::array_t<float> out({static_cast<py::ssize_t>(2), static_cast<py::ssize_t>(n)});
             float* d = out.mutable_data();
             p.process(d, d + n, n);
             p.collectGarbage();  // safe: the GIL serialises this with every other call
             return out;
           },
           py::arg("n"), "Render n frames: float32 ndarray of shape (2, n).")
      .def("play", [](PyStemPlayer& p) { p.need(); p.play(); })
      .def("pause", [](PyStemPlayer& p) { p.need(); p.pause(); })
      .def("seek", [](PyStemPlayer& p, long long pos) { p.need(); p.seek(pos); }, py::arg("pos"))
      .def("set_loop", [](PyStemPlayer& p, long long a, long long b) { p.need(); return p.setLoop(a, b); }, py::arg("a"), py::arg("b"))
      .def("clear_loop", [](PyStemPlayer& p) { p.need(); p.clearLoop(); })
      .def("set_count_in", [](PyStemPlayer& p, int bars, double bpm, int beatsPerBar) { p.need(); p.setCountIn(bars, bpm, beatsPerBar); },
           py::arg("bars"), py::arg("bpm"), py::arg("beats_per_bar") = 4)
      .def("set_click_level_db", [](PyStemPlayer& p, double db) { p.need(); p.setClickLevelDb(db); }, py::arg("db"))
      .def("set_stem_gain_db", [](PyStemPlayer& p, const std::string& k, double db) { p.need(); p.setStemGainDb(parseKind(k), db); }, py::arg("stem"), py::arg("db"))
      .def("set_stem_mute", [](PyStemPlayer& p, const std::string& k, bool v) { p.need(); p.setStemMute(parseKind(k), v); }, py::arg("stem"), py::arg("mute"))
      .def("set_stem_solo", [](PyStemPlayer& p, const std::string& k, bool v) { p.need(); p.setStemSolo(parseKind(k), v); }, py::arg("stem"), py::arg("solo"))
      .def("set_master_level_db", [](PyStemPlayer& p, double db) { p.need(); p.setMasterLevelDb(db); }, py::arg("db"))
      .def("set_guitar_mode", [](PyStemPlayer& p, const std::string& m) { p.need(); p.setGuitarMode(parseGuitarMode(m)); }, py::arg("mode"),
           "\"muted\" (default), \"ghost\" (-12 dB) or \"full\".")
      .def("set_rig_latency_samples", [](PyStemPlayer& p, int n) { p.need(); p.setRigLatencySamples(n); }, py::arg("samples"))
      .def("set_transport_mode", [](PyStemPlayer& p, const std::string& m) { p.need(); p.setTransportMode(parseMode(m)); }, py::arg("mode"),
           "\"free_run\" (default) or \"host_follow\".")
      .def("set_host_position", [](PyStemPlayer& p, long long pos, bool playing) { p.need(); p.setHostPosition(pos, playing); },
           py::arg("host_sample"), py::arg("host_playing"))
      .def("set_host_jump_threshold_samples", [](PyStemPlayer& p, long long t) { p.need(); p.setHostJumpThresholdSamples(t); }, py::arg("samples"))
      .def_property_readonly("position", [](const PyStemPlayer& p) { return p.position(); })
      .def_property_readonly("is_playing", [](const PyStemPlayer& p) { return p.isPlaying(); })
      .def_property_readonly("is_counting_in", [](const PyStemPlayer& p) { return p.isCountingIn(); })
      .def_property_readonly("at_end", [](const PyStemPlayer& p) { return p.atEnd(); })
      .def_property_readonly("stem_set_length", [](const PyStemPlayer& p) { return p.stemSetLength(); })
      .def_property_readonly("rig_latency_samples", [](const PyStemPlayer& p) { return p.rigLatencySamples(); })
      .def_property_readonly("latency_samples", [](const PyStemPlayer& p) { return p.latencySamples(); });
}

}  // namespace

PYBIND11_MODULE(sawblade_core, m) {
  m.doc() = "Sawblade core: offline preset renderer and capture cache";

  gPresetError = PyErr_NewExceptionWithDoc("sawblade_core.PresetError",
                                           "The preset is invalid. `json_path` names the offending member.",
                                           PyExc_ValueError, nullptr);
  gIoError = PyErr_NewExceptionWithDoc(
      "sawblade_core.RenderIOError",
      "A file could not be read, a capture failed to load or did not match its sha256. `json_path` is the "
      "preset path of the capture's `file` member when known.",
      PyExc_OSError, nullptr);
  m.attr("PresetError") = py::reinterpret_borrow<py::object>(gPresetError);
  m.attr("RenderIOError") = py::reinterpret_borrow<py::object>(gIoError);

  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    } catch (const RenderError& e) {
      raiseWithPath(e.kind() == RenderErrorKind::Preset ? gPresetError : gIoError, e.what(), e.jsonPath());
    } catch (const PresetError& e) {
      raiseWithPath(gPresetError, e.what(), e.jsonPath());
    } catch (const CaptureError& e) {
      raiseWithPath(gIoError, e.what(), e.jsonPath());
    }
  });

  py::class_<CaptureCache>(m, "CaptureCache",
                           "Loads each NAM model / IR once (keyed by path + sha256) and reuses it across renders.\n"
                           "Thread-safe; share one instance between threads.")
      .def(py::init<>())
      .def_property_readonly("hits", [](const CaptureCache& c) { return c.stats().hits; })
      .def_property_readonly("misses", [](const CaptureCache& c) { return c.stats().misses; })
      .def("__len__", [](const CaptureCache& c) { return c.stats().files; })
      .def("stats",
           [](const CaptureCache& c) {
             const auto s = c.stats();
             py::dict d;
             d["hits"] = s.hits;
             d["misses"] = s.misses;
             d["files"] = s.files;
             return d;
           })
      .def("reset_stats", &CaptureCache::resetStats, "Zero the hit/miss counters (keeps the data).")
      .def("clear", &CaptureCache::clear, "Drop all cached data and zero the counters.")
      .def("__repr__", [](const CaptureCache& c) {
        const auto s = c.stats();
        return "<CaptureCache files=" + std::to_string(s.files) + " hits=" + std::to_string(s.hits) +
               " misses=" + std::to_string(s.misses) + ">";
      });

  m.def("render", &render, py::arg("preset"), py::arg("audio"), py::arg("sample_rate"),
        py::arg("render_rate") = py::str("auto"), py::arg("out_rate") = "input", py::arg("block") = 256,
        py::arg("base_dir") = py::none(), py::arg("cache") = nullptr, py::arg("device_dbu") = py::none(),
        py::arg("di_channel") = "auto", py::arg("calibration") = "preset",
        R"doc(Render mono audio through a Sawblade preset (same code path as tonerender; bit-identical).

preset       JSON text (str) or a dict following docs/PRESET_SCHEMA.md.
audio        floating-point array (converted to float32; integer dtypes are rejected), 1-D mono or
             (frames, channels): one channel is rendered, chosen by di_channel (with a warning). A (1, N) array
             is rejected as probably channels-first: pass audio.T.
sample_rate  rate of `audio` in Hz (>= 1000).
render_rate  "auto" (the NAM models' training rate) or a rate in Hz.
out_rate     "input" (default; same length and rate as the input) or "render".
block        processing block size, 1..65536 (output does not depend on it).
base_dir     directory relative capture paths resolve against (default: the current directory).
cache        optional CaptureCache. Its invalidation is stat-gated (file size + mtime): an edit that keeps
             both unchanged is served stale; cache.clear() forces a reload.
device_dbu   v0.8 input calibration: the interface level, dBu at 0 dBFS (-60..60). None (default) = the assumed +12 dBu;
             the report records which (calibration.deviceDbu / deviceAssumed).
di_channel   "auto" (default: the louder of the first two channels by whole-file RMS, a tie picks L), "L", "R" or "mix"
             (mean of channels 0 and 1). Ignored for 1-D audio. The rule used is in report["diChannel"].
calibration  "preset" (default: follow the preset's calibration.mode; presets of version <= 4 are "legacy" = off and render
             bit-identically to before), "legacy" (force off) or "calibrated" (force on). report["calibration"] has the
             mode, the device level, whether it was assumed, and the per-block plan.

Returns (samples: float32 ndarray, report: dict). The report is the tonerender --report JSON
(latencySamples, pathLatency, renderRate, timings, warnings, ...). The GIL is released while
rendering. Raises PresetError (ValueError) or RenderIOError (OSError); both have `json_path`.)doc");

  m.def("level_match", &levelMatch, py::arg("preset"), py::arg("sample_rate"), py::arg("base_dir") = py::none(),
        py::arg("cache") = nullptr,
        py::arg("device_dbu") = py::none(), py::arg("calibration") = "preset",
        R"doc(Run the phase 10.1 level-match probe on a preset (builds the chain, prepare(), no audio rendered).

preset       JSON text (str) or a dict following docs/PRESET_SCHEMA.md; levelMatch.mode is forced to
             "auto" so the measured trims are returned whatever the preset says.
sample_rate  chain rate in Hz (use the NAM models' training rate, as render_rate="auto" does).
base_dir, cache  as in render().
device_dbu, calibration  as in render() (v0.8 I4a): the chain is calibrated before prepare(), so trims and make-up are
             measured on the chain the matcher renders. "preset" (default) follows the preset's calibration.mode.

Returns a dict: trimADb, trimBDb (dB, the louder path has 0), lufsA, lufsB, sumLufs (None when not
measured: a path disabled or silent), makeupDb (list of 5: make-up at blend 0, .25, .5, .75, 1),
delaySamplesB, invertB (the resolved alignment), warnings and calibrated (bool: the probe ran calibrated). With a path disabled everything is 0.
The GIL is released while measuring. Raises PresetError or RenderIOError like render().)doc");

  m.def("resolve_dynamics", &resolveDynamicsPy, py::arg("preset"), py::arg("base_dir") = py::none(),
        R"doc(The active dynamics set of a preset (core activeDynamics: dynamicsMode selects the record or the live set; the live
set is the explicit liveDynamics, else derived for origin "match", else the stored gate / busComp).

Returns {"mode": "live" | "record", "gate": {...}, "busComp": {...}} (docs/PRESET_SCHEMA.md objects). Raises PresetError.)doc");

  m.def("peak_floor_db", &peakFloorPy, py::arg("x"), py::arg("fs"), py::arg("key_hpf_hz") = 0.0, py::arg("mask") = py::none(),
        R"doc(The DI floor on the gate's own detector: the 92.5th percentile of the peak envelope (0.1 ms attack / 10 ms
release, after a key high-pass of key_hpf_hz when > 0) over the samples where `mask` is non-zero (all when None), in dBFS.
Returns None when the mask selects nothing.)doc");

  bindStems(m);
}
