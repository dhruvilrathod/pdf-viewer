#pragma once

// Detects whether this process is running from an installed MSIX package
// (Microsoft Store / sideloaded) as opposed to the plain portable .exe
// downloaded from GitHub. A handful of behaviors only make sense for one or
// the other -- see the call sites in viewer_window.cpp and updater.cpp.

namespace pkginfo {

// True when GetCurrentPackageFullName() reports a package identity, i.e. this
// binary is running inside its MSIX package. False (including on pre-MSIX
// Windows versions where the API doesn't apply) for the portable build.
bool IsPackaged();

} // namespace pkginfo
