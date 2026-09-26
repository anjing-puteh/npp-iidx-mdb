#include "musicdata_io.h"
#include "npp_defs.h"

#include <cstdint>
#include <exception>
#include <shlwapi.h>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma comment(lib, "Shlwapi.lib")

#define SCI_SETTEXT 2181
#define SCI_GETLENGTH 2006
#define SCI_GETTEXT 2182
#define SCI_SETLEXER 4001
#define SCLEX_NULL 1
#define SCI_GETCHARACTERPOINTER 2520

static const wchar_t kPluginName[] = L"MusicDataPlugin";

static NppData g_npp;
static std::unordered_set<UINT_PTR> g_musicBinBuffers;
static bool g_inPluginNotify = false;

struct PluginNotifyGuard {
  explicit PluginNotifyGuard(bool& flag) : flag_(flag), prev_(flag) { flag_ = true; }
  ~PluginNotifyGuard() { flag_ = prev_; }
  PluginNotifyGuard(const PluginNotifyGuard&) = delete;
  PluginNotifyGuard& operator=(const PluginNotifyGuard&) = delete;

 private:
  bool& flag_;
  bool prev_;
};

struct HandleCloser {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~HandleCloser() {
    if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
  }
  HandleCloser() = default;
  HandleCloser(const HandleCloser&) = delete;
  HandleCloser& operator=(const HandleCloser&) = delete;
  void release() { h = INVALID_HANDLE_VALUE; }
};

static bool restore_last_write_time(const wchar_t* path, const FILETIME& ft) {
  HandleCloser closer;
  closer.h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (closer.h == INVALID_HANDLE_VALUE) return false;
  BOOL ok = SetFileTime(closer.h, nullptr, nullptr, &ft);
  return ok != FALSE;
}

static bool path_is_music_bin(const wchar_t* path) {
  if (!path || !path[0]) return false;
  const wchar_t* slash = wcsrchr(path, L'\\');
  const wchar_t* fname = slash ? slash + 1 : path;
  return PathMatchSpecW(fname, L"music_*.bin") != FALSE;
}

static bool read_file_bytes(const wchar_t* path, std::vector<uint8_t>& out) {
  HandleCloser closer;
  closer.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (closer.h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(closer.h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (1LL << 31)) {
    return false;
  }
  try {
    out.resize(static_cast<size_t>(sz.QuadPart));
  } catch (...) {
    return false;
  }
  DWORD rd = 0;
  BOOL ok = ReadFile(closer.h, out.data(), static_cast<DWORD>(out.size()), &rd, nullptr);
  return ok && rd == out.size();
}

static bool write_file_bytes_atomic(const wchar_t* path, const std::vector<uint8_t>& data) {
  if (!path || !path[0]) return false;

  wchar_t dir[MAX_PATH];
  wcsncpy_s(dir, path, _TRUNCATE);
  PathRemoveFileSpecW(dir);
  if (!dir[0]) {
    if (!GetCurrentDirectoryW(MAX_PATH, dir)) return false;
  }

  wchar_t temp_path[MAX_PATH];
  if (!GetTempFileNameW(dir, L"mdb", 0, temp_path)) return false;

  bool wrote_ok = false;
  {
    HandleCloser closer;
    closer.h = CreateFileW(temp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (closer.h == INVALID_HANDLE_VALUE) {
      DeleteFileW(temp_path);
      return false;
    }
    DWORD wr = 0;
    BOOL ok = WriteFile(closer.h, data.data(), static_cast<DWORD>(data.size()), &wr, nullptr);
    if (ok && wr == data.size()) {
      FlushFileBuffers(closer.h);
      wrote_ok = true;
    }
  }
  if (!wrote_ok) {
    DeleteFileW(temp_path);
    return false;
  }

  if (!MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp_path);
    return false;
  }
  return true;
}

static bool get_buffer_full_path(UINT_PTR bufferID, std::wstring& out) {
  HWND npp = g_npp._nppHandle;
  int len = static_cast<int>(SendMessageW(npp, NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferID), 0));
  if (len <= 0) return false;
  out.assign(static_cast<size_t>(len) + 1, L'\0');
  int got = static_cast<int>(
      SendMessageW(npp, NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferID), reinterpret_cast<LPARAM>(out.data())));
  if (got <= 0) return false;
  out.resize(static_cast<size_t>(got));
  return true;
}

static bool activate_buffer(UINT_PTR bufferID) {
  HWND npp = g_npp._nppHandle;
  int pos = static_cast<int>(SendMessage(npp, NPPM_GETPOSFROMBUFFERID, static_cast<WPARAM>(bufferID), MAIN_VIEW));
  if (pos < 0) pos = static_cast<int>(SendMessage(npp, NPPM_GETPOSFROMBUFFERID, static_cast<WPARAM>(bufferID), SUB_VIEW));
  if (pos < 0) return false;
  unsigned up = static_cast<unsigned>(pos);
  int view = static_cast<int>(up >> 30);
  int index = static_cast<int>(up & 0x3FFFFFFFu);
  return SendMessage(npp, NPPM_ACTIVATEDOC, static_cast<WPARAM>(view), static_cast<LPARAM>(index)) == TRUE;
}

static HWND current_scintilla() {
  int which = 0;
  SendMessage(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
  return which ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}

static bool get_utf8_buffer_text(std::string& out) {
  HWND sci = current_scintilla();
  if (!sci) return false;
  LRESULT doc_len = SendMessage(sci, SCI_GETLENGTH, 0, 0);
  if (doc_len < 0) return false;
  size_t n = static_cast<size_t>(doc_len);
  if (n == 0) {
    out.clear();
    return true;
  }
  out.resize(n + 1, '\0');
  SendMessage(sci, SCI_GETTEXT, static_cast<WPARAM>(n + 1), reinterpret_cast<LPARAM>(&out[0]));
  out.resize(n);
  return true;
}

static bool get_document_span(const char*& ptr, size_t& len, std::string& owned_fallback) {
  HWND sci = current_scintilla();
  if (!sci) return false;
  const LRESULT doc_len = SendMessage(sci, SCI_GETLENGTH, 0, 0);
  if (doc_len < 0) return false;
  len = static_cast<size_t>(doc_len);
  if (len == 0) {
    ptr = "";
    return true;
  }
  const LRESULT raw = SendMessage(sci, SCI_GETCHARACTERPOINTER, 0, 0);
  ptr = reinterpret_cast<const char*>(static_cast<uintptr_t>(raw));
  if (ptr) return true;
  if (!get_utf8_buffer_text(owned_fallback)) return false;
  ptr = owned_fallback.data();
  len = owned_fallback.size();
  return true;
}

static bool set_buffer_utf8_text(UINT_PTR bufferID, const std::string& utf8) {
  if (!activate_buffer(bufferID)) return false;
  HWND sci = current_scintilla();
  if (!sci) return false;
  const LRESULT change_history_flags = SendMessage(sci, SCI_GETCHANGEHISTORY, 0, 0);
  SendMessage(sci, WM_SETREDRAW, static_cast<WPARAM>(FALSE), 0);
  SendMessage(sci, SCI_SETCHANGEHISTORY, static_cast<WPARAM>(SC_CHANGE_HISTORY_DISABLED), 0);
  SendMessage(sci, SCI_SETUNDOCOLLECTION, 0, 0);
  // Null lexer so SCI_SETTEXT does not lex the document that is about to be replaced.
  SendMessage(sci, SCI_SETLEXER, static_cast<WPARAM>(SCLEX_NULL), 0);
  SendMessage(sci, SCI_SETCODEPAGE, static_cast<WPARAM>(SC_CP_UTF8), 0);
  SendMessage(sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(utf8.c_str()));
  sci = current_scintilla();
  if (!sci) return false;
  SendMessage(sci, SCI_SETUNDOCOLLECTION, static_cast<WPARAM>(TRUE), 0);
  SendMessage(sci, SCI_SETSAVEPOINT, 0, 0);
  SendMessage(sci, SCI_EMPTYUNDOBUFFER, 0, 0);
  SendMessage(sci, SCI_SETCHANGEHISTORY, static_cast<WPARAM>(change_history_flags), 0);
  // NPPM_SETBUFFERENCODING only succeeds on an unmodified buffer. The menu command is the fallback.
  if (!SendMessage(g_npp._nppHandle, NPPM_SETBUFFERENCODING, static_cast<WPARAM>(bufferID), static_cast<LPARAM>(NPP_UNI_UTF8_NO_BOM))) {
    SendMessage(g_npp._nppHandle, NPPM_MENUCOMMAND, 0, static_cast<LPARAM>(NPP_IDM_FORMAT_AS_UTF_8));
  }
  SendMessage(g_npp._nppHandle, NPPM_SETBUFFERLANGTYPE, static_cast<WPARAM>(bufferID), static_cast<LPARAM>(NPP_L_JSON));
  sci = current_scintilla();
  if (sci) {
    SendMessage(sci, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessage(sci, SCI_SETSAVEPOINT, 0, 0);
    SendMessage(sci, WM_SETREDRAW, static_cast<WPARAM>(TRUE), 0);
    RedrawWindow(sci, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  }
  return true;
}

static void show_err(const wchar_t* msg) { MessageBoxW(g_npp._nppHandle, msg, kPluginName, MB_OK | MB_ICONWARNING); }

// PluginsManager requires _nbFuncItem > 0 (see Notepad++ PluginsManager.cpp).
static void cmd_about() {
  MessageBoxW(
      g_npp._nppHandle,
      L"Opens files matching music_*.bin as JSON and writes binary on save.\n"
      L"Tab may still show .bin; lexer is JSON.\n"
      L"Unknown regions match musicdata_tool.py fixed padding.\n"
      L"After save, the file's last-write time may match the JSON save (avoids reload prompts).",
      kPluginName,
      MB_OK | MB_ICONINFORMATION);
}

static FuncItem s_funcItems[] = {
    {L"About MusicDataPlugin...", cmd_about, 0, false, nullptr},
};

extern "C" __declspec(dllexport) void setInfo(NppData data) { g_npp = data; }

extern "C" __declspec(dllexport) const wchar_t* getName() { return kPluginName; }

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nb) {
  *nb = static_cast<int>(sizeof(s_funcItems) / sizeof(s_funcItems[0]));
  return s_funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notify) {
  if (!notify || g_inPluginNotify) return;
  PluginNotifyGuard guard(g_inPluginNotify);

  try {
    const int code = notify->nmhdr.code;
    const UINT_PTR bufferID = notify->nmhdr.idFrom;

    if (code == NPPN_FILEOPENED) {
      std::wstring path;
      if (!get_buffer_full_path(bufferID, path)) return;
      if (!path_is_music_bin(path.c_str())) return;

      std::vector<uint8_t> raw;
      if (!read_file_bytes(path.c_str(), raw)) return;
      if (raw.size() < 4 || memcmp(raw.data(), "IIDX", 4) != 0) return;

      std::string json;
      std::wstring err;
      if (!musicdata_extract_json(raw, json, err)) {
        show_err(err.empty() ? L"Failed to extract musicdata." : err.c_str());
        return;
      }

      if (!set_buffer_utf8_text(bufferID, json)) {
        show_err(L"Failed to load musicdata JSON into the editor.");
        return;
      }
      g_musicBinBuffers.insert(bufferID);
      return;
    }

    if (code == NPPN_FILECLOSED) {
      g_musicBinBuffers.erase(bufferID);
      return;
    }

    if (code == NPPN_FILESAVED) {
      if (g_musicBinBuffers.count(bufferID) == 0) return;
      if (!activate_buffer(bufferID)) {
        show_err(L"Could not activate document for save.");
        return;
      }
      const char* text = nullptr;
      size_t text_len = 0;
      std::string owned_text;
      if (!get_document_span(text, text_len, owned_text)) {
        show_err(L"Could not read editor text for save.");
        return;
      }
      std::vector<uint8_t> bin;
      std::wstring err;
      if (!musicdata_create_bin(text, text_len, bin, err)) {
        show_err(err.empty() ? L"Failed to create musicdata binary." : err.c_str());
        return;
      }
      std::wstring path;
      if (!get_buffer_full_path(bufferID, path)) {
        show_err(L"Could not resolve file path after save.");
        return;
      }
      bool have_ts = false;
      FILETIME ft_before{};
      WIN32_FILE_ATTRIBUTE_DATA fad{};
      if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ft_before = fad.ftLastWriteTime;
        have_ts = true;
      }
      if (!write_file_bytes_atomic(path.c_str(), bin)) {
        show_err(L"Failed to write musicdata .bin after save.");
        return;
      }
      if (have_ts && !restore_last_write_time(path.c_str(), ft_before)) {
        // Non-fatal: user may still see a reload prompt.
      }
    }
  } catch (const std::exception&) {
    show_err(L"MusicDataPlugin: unexpected error (out of memory or internal failure).");
  } catch (...) {
    show_err(L"MusicDataPlugin: unexpected error.");
  }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) { return FALSE; }

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
