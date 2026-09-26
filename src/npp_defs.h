#pragma once

#include <windows.h>

// Minimal Notepad++ plugin definitions (subset of PluginInterface / Notepad_plus_msgs.h)

#define NPPMSG (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_GETFULLPATHFROMBUFFERID (NPPMSG + 58)
#define NPPM_GETPOSFROMBUFFERID (NPPMSG + 57)
#define NPPM_ACTIVATEDOC (NPPMSG + 28)
#define NPPM_SETBUFFERLANGTYPE (NPPMSG + 65)
#define NPPM_GETBUFFERENCODING (NPPMSG + 66)
#define NPPM_SETBUFFERENCODING (NPPMSG + 67)
#define NPPM_MENUCOMMAND (NPPMSG + 48)

// UniMode: UTF-8 without BOM. JSON written by this plugin has no BOM.
#define NPP_UNI_UTF8_NO_BOM 4

// menuCmdID.h (Notepad++): Encoding -> UTF-8
#define NPP_IDM 40000
#define NPP_IDM_FORMAT (NPP_IDM + 5000)
#define NPP_IDM_FORMAT_AS_UTF_8 (NPP_IDM_FORMAT + 8)

// Scintilla (subset)
#define SCI_SETCODEPAGE 2079
#define SC_CP_UTF8 65001
#define SCI_SETSAVEPOINT 2019
#define SCI_SETUNDOCOLLECTION 2012
#define SCI_EMPTYUNDOBUFFER 2175
#define SCI_SETCHANGEHISTORY 2780
#define SCI_GETCHANGEHISTORY 2781

#define SC_CHANGE_HISTORY_DISABLED 0

#define MAIN_VIEW 0
#define SUB_VIEW 1

#define NPPN_FIRST 1000
#define NPPN_FILEOPENED (NPPN_FIRST + 4)
#define NPPN_FILECLOSED (NPPN_FIRST + 5)
#define NPPN_FILESAVED (NPPN_FIRST + 8)

struct NppData {
  HWND _nppHandle = nullptr;
  HWND _scintillaMainHandle = nullptr;
  HWND _scintillaSecondHandle = nullptr;
};

struct FuncItem {
  wchar_t _itemName[64] = {};
  void (*_pFunc)() = nullptr;
  int _cmdID = 0;
  bool _init2Check = false;
  void* _pShKey = nullptr;
};

// LangType order as in Notepad++ Notepad_plus_msgs.h (N++ 8.6+); verify if lexer wrong
enum NppLangType {
  NPP_L_TEXT = 0,
  NPP_L_JSON = 57
};

struct SCNotification {
  NMHDR nmhdr;
};
