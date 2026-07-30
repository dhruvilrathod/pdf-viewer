#include "package_info.h"

#include <windows.h>
#include <appmodel.h>

namespace pkginfo {

bool IsPackaged()
{
	UINT32 length = 0;
	LONG rc = GetCurrentPackageFullName(&length, nullptr);
	// APPMODEL_ERROR_NO_PACKAGE means "not running in a package identity" --
	// that's the only negative case. Any other return (typically
	// ERROR_INSUFFICIENT_BUFFER, since we passed a null buffer to just probe
	// the length) means a package identity exists.
	return rc != APPMODEL_ERROR_NO_PACKAGE;
}

} // namespace pkginfo
