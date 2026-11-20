#pragma once
// ============================================================================
// wadamesh remote-test controller — pure command parsing (no Arduino/LVGL).
//
// Kept dependency-free so the parser can be compiled and exercised on the host
// (tools/remote_test/parser_selftest.cpp) as well as in the firmware build.
//
// See docs/REMOTE_TEST.md for the full command reference and the line protocol.
// ============================================================================
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace remote_test {

enum class ParseResult : uint8_t {
  Ok,        // command + args parsed
  Empty,     // blank line or comment
  TooLong,   // over the 255-byte line budget
  NotACmd,   // first byte is not an ASCII letter (binary companion frame)
};

struct CommandLine {
  char command[24];
  char args[232];
};

// True when the byte can begin a text command. Companion-protocol frames start
// with '<' or a binary command byte (1..62), so a letter is unambiguous.
inline bool isCommandStart(int b) {
  if (b == '<') return false;
  return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}

inline bool isKnownCommand(const char* command) {
  if (!command) return false;
  static const char* const kCommands[] = {
      "help", "?", "ping", "screen", "nav", "navigate", "tab", "back", "tap",
      "key", "press", "type", "text", "status", "info", "tree", "widgets",
      "capture", "snapshot", "setrf", "display", "lock", "unlock", "wake",
      "reboot", "restart", "version",
  };
  for (const char* known : kCommands) {
    if (std::strcmp(command, known) == 0) return true;
  }
  return false;
}

// Lowercase a command + split args. Returns NotACmd when the first byte cannot
// start a text command (caller must leave those bytes for the mesh stack).
inline ParseResult parseLine(const char* line, CommandLine* out) {
  if (!line || !out) return ParseResult::Empty;
  while (*line && std::isspace(static_cast<unsigned char>(*line))) ++line;
  if (!*line || *line == '#' || *line == ';') return ParseResult::Empty;
  if (!isCommandStart(static_cast<unsigned char>(*line))) return ParseResult::NotACmd;

  const size_t total = std::strlen(line);
  if (total >= sizeof(out->command) + sizeof(out->args)) return ParseResult::TooLong;

  size_t n = 0;
  while (line[n] && !std::isspace(static_cast<unsigned char>(line[n])) &&
         n + 1 < sizeof(out->command)) {
    out->command[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(line[n])));
    ++n;
  }
  out->command[n] = '\0';
  while (line[n] && std::isspace(static_cast<unsigned char>(line[n]))) ++n;

  size_t a = 0;
  while (line[n] && a + 1 < sizeof(out->args)) {
    out->args[a++] = line[n++];
  }
  // Trim trailing whitespace.
  while (a > 0 && std::isspace(static_cast<unsigned char>(out->args[a - 1]))) --a;
  out->args[a] = '\0';
  return ParseResult::Ok;
}

// Parse an unsigned decimal argument. Returns false on garbage / overflow.
inline bool parseU32(const char* s, uint32_t* out, uint32_t max_value = 0xFFFFFFFFu) {
  if (!s || !*s || !out) return false;
  uint32_t value = 0;
  for (const char* p = s; *p; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (value > (max_value - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  *out = value;
  return true;
}

// Parse a signed decimal argument (no spaces). Returns false on garbage.
inline bool parseI32(const char* s, int32_t* out) {
  if (!s || !*s || !out) return false;
  bool neg = false;
  if (*s == '-') { neg = true; ++s; }
  if (!*s) return false;
  int32_t value = 0;
  for (const char* p = s; *p; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    const int32_t digit = static_cast<int32_t>(*p - '0');
    if (value > (2147483647 - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = neg ? -value : value;
  return true;
}

// Split the next whitespace-delimited token out of `args`. Returns nullptr when
// no tokens remain. `args` is advanced past the token.
inline char* nextToken(char** args) {
  if (!args || !*args) return nullptr;
  char* p = *args;
  while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
  if (!*p) { *args = p; return nullptr; }
  char* start = p;
  while (*p && !std::isspace(static_cast<unsigned char>(*p))) ++p;
  if (*p) { *p = '\0'; ++p; }
  *args = p;
  return start;
}

}  // namespace remote_test
