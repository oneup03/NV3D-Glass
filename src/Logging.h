/*
 * This file is part of NV3D-Glass.
 *
 * NV3D-Glass is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * NV3D-Glass is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with NV3D-Glass. If not, see <http://www.gnu.org/licenses/>.
 */

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
