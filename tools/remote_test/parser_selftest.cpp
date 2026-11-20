// Host-side selftest for the pure command parser (no Arduino / LVGL needed).
//   g++ -std=c++17 -O2 -o /tmp/rt_parser tools/remote_test/parser_selftest.cpp && /tmp/rt_parser
#include <cstdio>
#include <cstring>

#include "../../src/remote_test/RemoteTestCommands.h"

using namespace remote_test;

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

int main() {
  CommandLine cmd{};

  // Basic split + lowercase.
  CHECK(parseLine("NAV Home", &cmd) == ParseResult::Ok);
  CHECK(std::strcmp(cmd.command, "nav") == 0);
  CHECK(std::strcmp(cmd.args, "Home") == 0);

  // Leading/trailing whitespace.
  CHECK(parseLine("   ping   ", &cmd) == ParseResult::Ok);
  CHECK(std::strcmp(cmd.command, "ping") == 0);
  CHECK(cmd.args[0] == '\0');

  // Blank / comment lines.
  CHECK(parseLine("", &cmd) == ParseResult::Empty);
  CHECK(parseLine("   ", &cmd) == ParseResult::Empty);
  CHECK(parseLine("# nope", &cmd) == ParseResult::Empty);
  CHECK(parseLine("; nope", &cmd) == ParseResult::Empty);

  // Companion frames and binary bytes must be left for the mesh stack.
  CHECK(parseLine("<frame", &cmd) == ParseResult::NotACmd);
  CHECK(parseLine("\x01\x02", &cmd) == ParseResult::NotACmd);

  // Overlong line.
  char big[300];
  std::memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  CHECK(parseLine(big, &cmd) == ParseResult::TooLong);

  // Command table.
  CHECK(isKnownCommand("nav"));
  CHECK(isKnownCommand("capture"));
  CHECK(!isKnownCommand("bogus"));

  // Numeric parsing.
  uint32_t u = 0;
  CHECK(parseU32("320", &u) && u == 320);
  CHECK(!parseU32("-1", &u));
  CHECK(!parseU32("12x", &u));
  CHECK(!parseU32("", &u));
  CHECK(!parseU32("999999999999", &u));

  int32_t i = 0;
  CHECK(parseI32("-9", &i) && i == -9);
  CHECK(parseI32("22", &i) && i == 22);
  CHECK(!parseI32("--2", &i));

  // Token walking.
  char args[] = " 160  120 250 ";
  char* p = args;
  char* t1 = nextToken(&p);
  char* t2 = nextToken(&p);
  char* t3 = nextToken(&p);
  char* t4 = nextToken(&p);
  CHECK(t1 && std::strcmp(t1, "160") == 0);
  CHECK(t2 && std::strcmp(t2, "120") == 0);
  CHECK(t3 && std::strcmp(t3, "250") == 0);
  CHECK(t4 == nullptr);

  if (failures == 0) std::printf("parser selftest: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
