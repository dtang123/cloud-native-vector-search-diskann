// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.
#pragma once
#include <string>

inline std::string resolve_local_path(const std::string &path)
{
    if (path.rfind("s3://", 0) == 0)
    {
        size_t last_slash = path.rfind('/');
        return "/dev/shm/diskann_metadata/" + path.substr(last_slash + 1);
    }
    return path;
}
