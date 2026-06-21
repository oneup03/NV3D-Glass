#pragma once

#include "NV3D.hpp"   // NV3D::LogLevel

namespace nv3dg {

void InitFileLog(const wchar_t* path);
void ShutdownFileLog();

// Write a formatted line to the SAME sink NV3DLib routes its messages
// through (NV3D-Glass.log + OutputDebugString). Use this rather than
// OutputDebugStringW so diagnostic output ends up alongside [NV3D][...]
// lines in the on-disk log file.
void Log(NV3D::LogLevel level, const wchar_t* fmt, ...);

}
