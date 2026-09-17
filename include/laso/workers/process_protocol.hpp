#pragma once

#include <cstddef>
#include <cstdint>

namespace laso::process_protocol {
// Version 1 is newline-delimited JSON (NDJSON). One request produces exactly
// one response; unsolicited messages are not part of the protocol.
inline constexpr std::uint32_t version = 1;
inline constexpr std::size_t max_frame_bytes = std::size_t{1024} * 1024;
inline constexpr std::size_t max_stderr_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t max_metadata_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t max_artifact_references = 16;
inline constexpr std::size_t max_outstanding_requests = 1;
} // namespace laso::process_protocol
