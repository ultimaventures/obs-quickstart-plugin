// Pre-harness seed for the copyDirectoryTree unit test (reviewer round 4).
//
// This standalone program reproduces the two filesystem behaviors the
// profile module depends on. It is committed as the starting point for the
// test-harness task (first task of the next sprint): port these cases into
// the gtest target, replacing main() with TEST() blocks and the blog()
// stub described in plans/02-test-harness-backup-location.md.
//
// Build: g++ -std=c++17 -o fstest tests/manual/filesystem-repro.cpp
// Run:   ./fstest   (uses a mkdtemp sandbox; prints PASS/FAIL per case)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond, label)                                                  \
  do {                                                                     \
    if (cond) {                                                            \
      std::printf("PASS: %s\n", label);                                    \
    } else {                                                               \
      std::printf("FAIL: %s\n", label);                                    \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

// Mirrors ProfileManager's copyDirectoryTree error handling: establish
// missing parents with create_directories(error_code), then copy.
static bool copyTree(const fs::path &source, const fs::path &destination) {
  try {
    const fs::path parent = destination.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      fs::create_directories(parent, ec);
      if (ec)
        return false;
    }
    fs::copy(source, destination,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  } catch (const fs::filesystem_error &) {
    return false;
  }
  return true;
}

int main() {
  // Sandbox so the repro never touches real data.
  char tmpl[] = "/tmp/fstest-XXXXXX";
  if (!mkdtemp(tmpl)) {
    std::printf("FAIL: mkdtemp\n");
    return 1;
  }
  const fs::path sandbox = fs::u8path(tmpl);

  const fs::path srcDir = sandbox / "src-profile";
  fs::create_directories(srcDir);
  {
    std::ofstream f((srcDir / "basic.ini").u8string());
    f << "[General]\nName=Test\n";
  }

  // Case 1 (reviewer round 3): copy with a missing destination parent must
  // succeed once parents are established (std::filesystem::copy alone
  // throws "No such file or directory" here).
  {
    const fs::path dest = sandbox / "missing-parent" / "backup_123";
    CHECK(copyTree(srcDir, dest) && fs::exists(dest / "basic.ini"),
          "copy succeeds after create_directories establishes parents");
  }

  // Case 2 (reviewer round 4): non-ASCII (UTF-8) directory names must round-
  // trip. u8path decodes UTF-8 on every platform; path(string) on Windows
  // would decode with the ANSI code page instead.
  {
    const fs::path dest =
        fs::u8path(sandbox.u8string() + "/backups-\u00e9\u00e8") / "bk_1";
    CHECK(copyTree(srcDir, dest) && fs::exists(dest / "basic.ini"),
          "copy into non-ASCII (UTF-8) directory succeeds");
    CHECK(dest.u8string().find("\u00e9") != std::string::npos,
          "u8string() preserves the non-ASCII name");
  }

  // Case 3: nested content is copied recursively, existing files overwritten.
  {
    fs::create_directories(srcDir / "sub");
    {
      std::ofstream f((srcDir / "sub" / "deep.txt").u8string());
      f << "deep";
    }
    const fs::path dest = sandbox / "nested" / "bk_2";
    CHECK(copyTree(srcDir, dest) && fs::exists(dest / "sub" / "deep.txt"),
          "recursive copy preserves nested content");
  }

  std::error_code ec;
  fs::remove_all(sandbox, ec);
  std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
  return failures == 0 ? 0 : 1;
}
