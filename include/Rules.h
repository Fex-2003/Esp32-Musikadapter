#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Hardwareunabhaengige Regeln; mit test/rules_test.cpp am PC testbar.
namespace rules {
bool utf8(const std::string& s, size_t* utf16Units = nullptr);
bool motionFile(const std::string& filename);
bool playlistName(const std::string& name);
bool musicFile(const std::string& filename);
bool btName(const std::string& name);
bool weight(const std::string& text, double& out);
bool unsignedNumber(const std::string& text, uint32_t max, uint32_t& out);
bool mp3Extension(const std::string& filename);
// Byteweise alphabetisch; zusammenhaengende ASCII-Ziffern werden als Zahl verglichen.
int naturalCompare(const std::string& left, const std::string& right);
std::string html(const std::string& text);
std::string json(const std::string& text);
std::string url(const std::string& text);
int weightedPick(const std::vector<double>& weights, int last, uint32_t random);
uint32_t below(uint32_t n, uint32_t (*rng)());
void order(std::vector<uint16_t>& result, size_t count, bool shuffle,
           int previous, uint32_t (*rng)());
}
