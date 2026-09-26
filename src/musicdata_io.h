#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// IIDX musicdata.bin <-> JSON (matches musicdata_tool.py handlers / reader / writer).

bool musicdata_extract_json(const std::vector<uint8_t>& bin, std::string& out_json_utf8, std::wstring& err);

bool musicdata_create_bin(const char* json_utf8, size_t json_len, std::vector<uint8_t>& out_bin, std::wstring& err);

bool musicdata_create_bin(const std::string& json_utf8, std::vector<uint8_t>& out_bin, std::wstring& err);
