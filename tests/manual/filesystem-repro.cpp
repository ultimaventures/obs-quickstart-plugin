// Manual repro / verification for the UTF-8 path handling used by
// backupExistingProfile() and copyDirectoryTree() in
// src/profile/profile-module.cpp.
//
// Build: g++ -std=c++17 -Wall -Wextra -o /tmp/fstest
//          tests/manual/filesystem-repro.cpp
// Run:   /tmp/fstest   (exits 0 on success, 1 on any failure)
//
// IMPORTANT CAVEAT: on Linux/macOS, path(std::string) already treats narrow
// strings as UTF-8, so the non-ASCII cases below pass WITH or WITHOUT u8path.
// They only guard the Windows behavior when this program runs on the Windows
// CI leg, where path(string) decodes with the ANSI code page. The Windows leg
// must run these (see plans/02-test-harness-backup-location.md).

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char *name) {
  std::cout << (ok ? "PASS" : "FAIL") << ": " << name << '\n';
  if (!ok)
    ++failures;
}

// Mirrors copyDirectoryTree() in profile-module.cpp: destination arrives as
// a UTF-8 std::string (dest.u8string()), gets wrapped in u8path, missing
// parents are created first, then a recursive copy.
bool copyTreeUtf8(const std::string &source, const std::string &destination) {
  try {
    const fs::path parent = fs::u8path(destination).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      fs::create_directories(parent, ec);
      if (ec)
        return false;
    }
    fs::copy(fs::u8path(source), fs::u8path(destination),
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);
  } catch (const fs::filesystem_error &) {
    return false;
  }
  return true;
}

void writeFile(const fs::path &p, const std::string &content) {
  // Open via the path object itself, never via .u8string(): the narrow
  // string overload would re-decode with the ANSI code page on Windows.
  std::ofstream out(p, std::ios::binary);
  out << content;
}

std::string readFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

} // namespace

int main() {
  // Portable sandbox: temp_directory_path(), no /tmp, no mkdtemp.
  const fs::path sandbox = fs::temp_directory_path() / "quickstart-fstest";
  std::error_code ec;
  fs::remove_all(sandbox, ec);

  // 1. Missing parents are created before the copy.
  {
    const fs::path src = sandbox / "profile";
    fs::create_directories(src / "sub");
    writeFile(src / "basic.ini", "name=Quickstart\n");
    const std::string dest =
        (sandbox / "quickstart-backups" / "deep" / "missing" / "profile_123")
            .u8string();
    check(copyTreeUtf8(src.u8string(), dest),
          "create_directories builds missing parents");
  }

  // 2. Nested recursive copy preserves structure and file bytes.
  {
    const fs::path src = sandbox / "profile2";
    fs::create_directories(src / "a" / "b");
    writeFile(src / "basic.ini", "name=Quickstart\n");
    writeFile(src / "a" / "b" / "deep.txt", "deep-content");
    const fs::path dest = sandbox / "backup2" / "profile2_456";
    const bool ok = copyTreeUtf8(src.u8string(), dest.u8string());
    check(ok && readFile(dest / "basic.ini") == "name=Quickstart\n" &&
              readFile(dest / "a" / "b" / "deep.txt") == "deep-content",
          "recursive copy preserves nested structure and bytes");
  }

  // 3. Non-ASCII directory name round-trips (u8 literal: MSVC's default
  // execution charset is ANSI without /utf-8, so "\u00e9" would be wrong).
  {
    const fs::path cafe = sandbox / fs::u8path(u8"Caf\u00e9");
    fs::create_directories(cafe, ec);
    check(!ec && fs::is_directory(cafe) &&
              fs::u8path(cafe.u8string()).filename().u8string() ==
                  fs::u8path(u8"Caf\u00e9").u8string(),
          "non-ASCII directory create/list round-trip");
  }

  // 4. std::ofstream(fs::path) writes inside the non-ASCII directory.
  {
    const fs::path cafe = sandbox / fs::u8path(u8"Caf\u00e9");
    writeFile(cafe / "note.txt", "caf\u00e9-data");
    check(readFile(cafe / "note.txt") == "caf\u00e9-data",
          "ofstream(path) writes into non-ASCII directory");
  }

  // 5. Backup-destination construction with a non-ASCII profile name, exactly
  // as backupExistingProfile() builds it: every component from OBS strings
  // goes through u8path (operator/ with a plain std::string would decode the
  // name with the Windows ANSI code page and mangle "Caf\u00e9").
  {
    const std::string profilePath =
        (sandbox / fs::u8path(u8"Caf\u00e9")).u8string();
    const std::string profileName = u8"Caf\u00e9";
    const std::string stamp = "123";
    const fs::path dest = fs::u8path(profilePath).parent_path() /
                          "quickstart-backups" /
                          fs::u8path(profileName + "_" + stamp);
    const fs::path expect =
        sandbox / "quickstart-backups" / fs::u8path(u8"Caf\u00e9_123");
    check(dest.u8string() == expect.u8string(),
          "backup destination keeps non-ASCII profile name intact");
  }

  // 6. End-to-end: copy a profile tree into the non-ASCII backup destination.
  {
    const fs::path src = sandbox / fs::u8path(u8"Caf\u00e9");
    writeFile(src / "basic.ini", "name=Caf\u00e9\n");
    const std::string profilePath = src.u8string();
    const std::string profileName = u8"Caf\u00e9";
    const fs::path dest = fs::u8path(profilePath).parent_path() /
                          "quickstart-backups" /
                          fs::u8path(profileName + "_789");
    const bool ok = copyTreeUtf8(profilePath, dest.u8string());
    check(ok && readFile(dest / "basic.ini") == "name=Caf\u00e9\n",
          "profile copy into non-ASCII backup destination");
  }

  fs::remove_all(sandbox, ec);
  if (failures)
    std::cout << "FAILURES: " << failures << '\n';
  else
    std::cout << "ALL PASS (6 checks)\n";
  return failures ? 1 : 0;
}
