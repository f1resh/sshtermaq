#pragma once
// Which OS and CPU this build is for (app:info, logs). Portable code never branches on the OS; it calls a platform/
// function whose implementation differs instead (see README.md in this folder).

#include <string>

namespace st::sys {

/** "win32" or "darwin" (the names the UI uses in app:info). */
const char* osName();
/** "x64" or "arm64". */
const char* archName();
/** The running OS and its version for the app log: "Windows 10.0.26100", "macOS 15.1". */
std::string osVersion();
/**
 * How this copy was installed, for the version check (app/UpdateCheck.h):
 * - "store": a Microsoft Store (MSIX) package; the Store updates it, so there is no check;
 * - "homebrew": a Homebrew cask (its folder in Caskroom exists); the advice is `brew upgrade --cask sshtermaq`;
 * - "other": the installer (also through winget), the zip, a portable copy, the disk image.
 */
const char* installChannel();

}  // namespace st::sys
