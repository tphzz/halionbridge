#pragma once

#include "halionbridge/halionbridge_export.h"

namespace halionbridge
{

struct HALIONBRIDGE_EXPORT BuildInfo
{
    const char* versionString;
    const char* packageBasename;
    const char* gitTag;
    const char* gitBranch;
    const char* gitShaShort;
    const char* buildTimestampUtc;
    bool isTaggedRelease;
    bool isDirty;
};

HALIONBRIDGE_EXPORT BuildInfo getBuildInfo() noexcept;

} // namespace halionbridge
