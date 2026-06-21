#include "Logging.h"

#include "NV3D.hpp"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace nv3dg {

namespace {

FILE*       g_log = nullptr;
std::mutex  g_mtx;

void LogSink(NV3D::LogLevel level, const wchar_t* msg, void*) {
    const wchar_t* lvl = L"?";
    switch (level) {
        case NV3D::LogLevel::Debug:   lvl = L"D"; break;
        case NV3D::LogLevel::Info:    lvl = L"I"; break;
        case NV3D::LogLevel::Warning: lvl = L"W"; break;
        case NV3D::LogLevel::Error:   lvl = L"E"; break;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_log) {
            fwprintf(g_log, L"[NV3D][%s] %s\n", lvl, msg);
            fflush(g_log);
        }
    }
    wchar_t buf[2048];
    _snwprintf_s(buf, _TRUNCATE, L"[NV3D-Glass][%s] %s\n", lvl, msg);
    OutputDebugStringW(buf);
}

}

void Log(NV3D::LogLevel level, const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    LogSink(level, buf, nullptr);
}

void InitFileLog(const wchar_t* path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_log) { fclose(g_log); g_log = nullptr; }
    _wfopen_s(&g_log, path, L"w, ccs=UTF-16LE");
    NV3D::SetLogSink(LogSink, nullptr);
}

void ShutdownFileLog() {
    NV3D::SetLogSink(nullptr, nullptr);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

}
