// Force-included into Play!'s Framework (engine/CMakeLists.txt) so the pinned third-party source builds unmodified with
// newer Windows SDKs. Framework's PathUtils.cpp tests `WINAPI_FAMILY_ONE_PARTITION(WINAPI_FAMILY, WINAPI_PARTITION_APP)`
// (a Store app?). Older SDKs defined that macro in <winapifamily.h> (and the standard headers of Visual Studio 2022
// included it); with Visual Studio 2026 it is not defined there. This is a desktop program: the answer is "no".
#pragma once
#ifdef _WIN32
#include <winapifamily.h>
#ifndef WINAPI_FAMILY_ONE_PARTITION
#define WINAPI_FAMILY_ONE_PARTITION(PartitionSet, Partition) 0
#endif
#endif
