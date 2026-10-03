// sawblade_core: Python bindings for the offline renderer (spec: docs/specs/phase3_matcher.md 3.1).
//
//   render(preset, audio, sample_rate, render_rate="auto", out_rate="input", block=256,
//          base_dir=None, cache=None) -> (float32 ndarray, report dict)
//   CaptureCache: loads each .nam / IR once, shared by any number of renders and threads.
//
// The GIL is released for the whole parse + render; nothing in there touches Python objects.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <cmath>
#include <exception>
#include <string>

#include "sawblade/capture_cache.h"
#include "sawblade/render.h"

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
                 const py::object& baseDir, CaptureCache* cache) {
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
        py::arg("base_dir") = py::none(), py::arg("cache") = nullptr,
        R"doc(Render mono audio through a Sawblade preset (same code path as tonerender; bit-identical).

preset       JSON text (str) or a dict following docs/PRESET_SCHEMA.md.
audio        floating-point array (converted to float32; integer dtypes are rejected), 1-D mono or
             (frames, channels) (first channel used, with a warning). A (1, N) array is rejected as
             probably channels-first: pass audio.T.
sample_rate  rate of `audio` in Hz (>= 1000).
render_rate  "auto" (the NAM models' training rate) or a rate in Hz.
out_rate     "input" (default; same length and rate as the input) or "render".
block        processing block size, 1..65536 (output does not depend on it).
base_dir     directory relative capture paths resolve against (default: the current directory).
cache        optional CaptureCache. Its invalidation is stat-gated (file size + mtime): an edit that keeps
             both unchanged is served stale; cache.clear() forces a reload.

Returns (samples: float32 ndarray, report: dict). The report is the tonerender --report JSON
(latencySamples, pathLatency, renderRate, timings, warnings, ...). The GIL is released while
rendering. Raises PresetError (ValueError) or RenderIOError (OSError); both have `json_path`.)doc");
}
