// FastMCPP C++ Port — Version Header
#pragma once

namespace fastmcpp
{
/// The first three components mirror the upstream fastmcp release this port is at
/// parity with, so they answer "which fastmcp is this?".
constexpr int VERSION_MAJOR = FASTMCPP_VERSION_MAJOR;
constexpr int VERSION_MINOR = FASTMCPP_VERSION_MINOR;
constexpr int VERSION_PATCH = FASTMCPP_VERSION_PATCH;

/// Local revision on top of that parity level, for C++-side work with no upstream
/// delta. Zero when the port sits exactly on an upstream release; it resets when
/// the upstream anchor moves.
constexpr int VERSION_TWEAK = FASTMCPP_VERSION_TWEAK;

/// Dotted four-component version, e.g. "3.4.7.1".
///
/// This is what goes on the wire as `clientInfo.version` during initialize. It is
/// assembled by CMake rather than here so there is exactly one place the format
/// lives -- the string was previously hardcoded at the handshake site and drifted
/// several releases behind the library.
constexpr const char* VERSION_STRING = FASTMCPP_VERSION_STRING;
} // namespace fastmcpp
