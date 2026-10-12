#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace sawblade {

// Self-contained SHA-256 (FIPS 180-4), written for Sawblade (no third-party code).
// Load-time only. Returns 64 lowercase hex characters.
std::string sha256Hex(const void* data, std::size_t size);

// Hashes a whole file. Throws std::runtime_error (path in message) if it cannot be read.
std::string sha256File(const std::filesystem::path& path);

}  // namespace sawblade
