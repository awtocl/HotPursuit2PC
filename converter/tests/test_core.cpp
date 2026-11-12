// Converter-core tests on a synthetic disc (no game data): ROM validation
// messages, game-folder rules (C2, C6), resume/idempotency (brief §3.6).
#include "minitest.h"
#include "testkit.h"

#include <map>

#include "hp2/convert.h"
#include "hp2/fileset.h"
#include "hp2/io.h"

using namespace hp2;
using testkit::pattern;
using testkit::TempDir;

namespace {

Bytes bytes_of(std::string_view s) { return Bytes(s.begin(), s.end()); }

bool contains(const std::vector<std::string>& lines, std::string_view needle) {
  for (const std::string& l : lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// name -> (size, last write time) of every file below dir: proves "nothing changed".
using Snapshot = std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>>;
Snapshot snapshot(const fs::path& dir) {
  Snapshot s;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return s;
  for (const auto& de : fs::recursive_directory_iterator(dir))
    if (de.is_regular_file())
      s[fs::relative(de.path(), dir).generic_string()] = {de.file_size(), de.last_write_time()};
  return s;
}

// A tiny stand-in for the extracted disc, with a matching profile.
struct FakeDisc {
  TempDir tmp{"core"};
  fs::path rom = tmp.path() / "Fake Disc (USA)";
  RomProfile profile;

  FakeDisc() {
    testkit::write_bytes(rom / "SYSTEM.CNF",
                         bytes_of("BOOT2 = cdrom0:\\SLUS_203.62;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n"));
    testkit::write_bytes(rom / "SLUS_203.62", pattern(4000, 11));
    testkit::write_synthetic_fileset(rom, {{name_hash("GLOBAL\\GLOBALA.BUN"), {}},
                                           {name_hash("GLOBAL\\HUDTEX.BIN"), pattern(5000, 12)},
                                           {name_hash("NFS6.ICO"), pattern(700, 13)},
                                           {0xDEADBEEFu, pattern(2048, 14)}});
    testkit::write_bytes(rom / "NFSHP2" / "IRX243" / "PADMAN.IRX", pattern(900, 15));
    testkit::write_bytes(rom / "NFSHP2" / "IRX243" / "VSSVER.SCC", pattern(208, 16));

    profile.serial = "SLUS-20362";
    profile.title = "synthetic test disc";
    profile.boot_file = "SLUS_203.62";
    profile.version = "1.00";
    profile.video_mode = "NTSC";
    for (const char* rel : {"NFSHP2/IRX243/PADMAN.IRX", "NFSHP2/IRX243/VSSVER.SCC", "NFSHP2/ZDIR.BIN",
                            "NFSHP2/ZZDATA.BIN", "SLUS_203.62", "SYSTEM.CNF"}) {
      const Bytes data = testkit::read_bytes(rom / rel);
      RomManifestEntry e;
      e.path = rel;
      e.size = data.size();
      e.sha256 = Sha256::hex_of(data);
      e.optional = e.path.ends_with("VSSVER.SCC");
      profile.files.push_back(e);
    }
  }

  ConvertRequest request(const fs::path& game) const {
    ConvertRequest r;
    r.rom_dir = rom;
    r.game_dir = game;
    return r;
  }
};

}  // namespace

// ------------------------------------------------------------------ check_rom

TEST(rom_check_accepts_a_complete_disc_and_writes_nothing) {
  FakeDisc d;
  const Snapshot before = snapshot(d.rom);
  const RomReport r = check_rom(d.rom, RomCheckLevel::Quick, d.profile);
  CHECK(r.ok());
  CHECK(r.fully_hashed);
  CHECK(r.warnings.empty());
  CHECK_EQ(r.files.size(), static_cast<std::size_t>(6));
  for (const RomFileResult& f : r.files) CHECK(f.state == RomFileState::Ok);
  CHECK(r.summary().find("result: valid") != std::string::npos);
  CHECK(snapshot(d.rom) == before);
}

TEST(rom_check_reports_missing_truncated_and_modified_files) {
  FakeDisc d;
  std::error_code ec;
  fs::rename(d.rom / "NFSHP2" / "IRX243" / "PADMAN.IRX", d.tmp.path() / "moved.bin", ec);
  testkit::write_bytes(d.rom / "SLUS_203.62", pattern(3999, 11));  // truncated
  Bytes flipped = testkit::read_bytes(d.rom / "NFSHP2" / "ZZDATA.BIN");
  flipped[10] ^= 0xFF;                                             // same size, other bytes
  testkit::write_bytes(d.rom / "NFSHP2" / "ZZDATA.BIN", flipped);

  const RomReport r = check_rom(d.rom, RomCheckLevel::Quick, d.profile);
  CHECK(!r.ok());
  CHECK(contains(r.errors, "NFSHP2/IRX243/PADMAN.IRX is missing"));
  CHECK(contains(r.errors, "SLUS_203.62 is 3999 bytes but the SLUS-20362 disc has 4000"));
  CHECK(contains(r.errors, "NFSHP2/ZZDATA.BIN has the right size but different contents"));
  CHECK(contains(r.errors, "1 required file(s) are missing"));
  CHECK(r.summary().find("NOT usable") != std::string::npos);
}

TEST(rom_check_treats_disc_leftovers_as_optional) {
  FakeDisc d;
  std::error_code ec;
  fs::rename(d.rom / "NFSHP2" / "IRX243" / "VSSVER.SCC", d.tmp.path() / "moved.scc", ec);
  const RomReport r = check_rom(d.rom, RomCheckLevel::Quick, d.profile);
  CHECK(r.ok());
  CHECK(contains(r.warnings, "VSSVER.SCC is missing"));
}

TEST(rom_check_names_the_wrong_disc) {
  FakeDisc d;
  testkit::write_bytes(d.rom / "SYSTEM.CNF", bytes_of("BOOT2 = cdrom0:\\SLES_507.31;1\r\nVER = 1.00\r\nVMODE = PAL\r\n"));
  const RomReport r = check_rom(d.rom, RomCheckLevel::Quick, d.profile);
  CHECK(!r.ok());
  CHECK(contains(r.errors, "SLES-50731"));
  CHECK(contains(r.errors, "Only SLUS-20362"));
  CHECK_EQ(serial_from_boot_file("SLUS_203.62"), std::string("SLUS-20362"));
  CHECK_EQ(serial_from_boot_file("weird"), std::string("weird"));
}

TEST(rom_check_explains_wrong_kinds_of_input) {
  FakeDisc d;
  // nothing there
  RomReport r = check_rom(d.tmp.path() / "nope", RomCheckLevel::Quick, d.profile);
  CHECK(contains(r.errors, "does not exist"));
  // empty path
  r = check_rom(fs::path(), RomCheckLevel::Quick, d.profile);
  CHECK(contains(r.errors, "No ROM folder was given"));
  // a disc image instead of a folder
  testkit::write_bytes(d.tmp.path() / "game.iso", pattern(64, 1));
  r = check_rom(d.tmp.path() / "game.iso", RomCheckLevel::Quick, d.profile);
  CHECK(contains(r.errors, "is a disc image file"));
  CHECK(contains(r.errors, "extract"));
  // a folder that is not a PS2 disc
  testkit::write_bytes(d.tmp.path() / "other" / "readme.txt", pattern(10, 1));
  r = check_rom(d.tmp.path() / "other", RomCheckLevel::Quick, d.profile);
  CHECK(contains(r.errors, "SYSTEM.CNF is missing"));
  // SYSTEM.CNF without BOOT2
  testkit::write_bytes(d.rom / "SYSTEM.CNF", bytes_of("hello\r\n"));
  r = check_rom(d.rom, RomCheckLevel::Quick, d.profile);
  CHECK(contains(r.errors, "no BOOT2 line"));
}

TEST(rom_check_corrects_a_neighbouring_folder) {
  FakeDisc d;
  // the NFSHP2 subfolder was picked
  RomReport r = check_rom(d.rom / "NFSHP2", RomCheckLevel::Quick, d.profile);
  CHECK(r.ok());
  CHECK(fs::equivalent(r.root, d.rom));
  CHECK(contains(r.notes, "using its parent"));
  // the folder above the disc was picked (it holds exactly one disc)
  r = check_rom(d.tmp.path(), RomCheckLevel::Quick, d.profile);
  CHECK(r.ok());
  CHECK(fs::equivalent(r.root, d.rom));
  CHECK(contains(r.notes, "in its subfolder"));
}

TEST(rom_check_detects_an_inconsistent_archive) {
  FakeDisc d;
  // Make ZDIR point outside ZZDATA, and update the profile so only the
  // structural check can notice.
  Bytes zdir = testkit::read_bytes(d.rom / "NFSHP2" / "ZDIR.BIN");
  zdir[12 + 9] = 0xFF;  // record 1: size far beyond the data file
  testkit::write_bytes(d.rom / "NFSHP2" / "ZDIR.BIN", zdir);
  for (RomManifestEntry& e : d.profile.files)
    if (e.path == "NFSHP2/ZDIR.BIN") e.sha256 = Sha256::hex_of(zdir);
  const RomReport r = check_rom(d.rom, RomCheckLevel::Quick, d.profile);
  CHECK(!r.ok());
  CHECK(contains(r.errors, "game data archive is inconsistent"));
}

TEST(builtin_profile_describes_slus20362) {
  const RomProfile& p = RomProfile::slus20362();
  CHECK_EQ(p.serial, std::string("SLUS-20362"));
  CHECK_EQ(p.files.size(), static_cast<std::size_t>(27));
  CHECK_EQ(p.total_size(), static_cast<std::uint64_t>(3120519117ull));  // sum of the 27 files on the disc
  REQUIRE(p.find("NFSHP2/ZZDATA.BIN") != nullptr);
  CHECK_EQ(p.find("NFSHP2/ZZDATA.BIN")->size, static_cast<std::uint64_t>(3116437504ull));
  REQUIRE(p.find("NFSHP2/IRX243/VSSVER.SCC") != nullptr);
  CHECK(p.find("NFSHP2/IRX243/VSSVER.SCC")->optional);
  std::size_t optional = 0;
  for (const RomManifestEntry& e : p.files) {
    CHECK_EQ(e.sha256.size(), static_cast<std::size_t>(64));
    if (e.optional) ++optional;
  }
  CHECK_EQ(optional, static_cast<std::size_t>(1));
}

// ------------------------------------------------------------- small pieces

TEST(key_value_file_roundtrips) {
  TempDir tmp("kv");
  KeyValueFile kv;
  kv.set("mode", "portable");
  kv.set("rom_source", "D:\\Games\\My ROM = here");
  kv.set("multi", "line one\nline two");
  kv.save(tmp.path() / "sub" / "hp2pc.cfg");
  const KeyValueFile back = KeyValueFile::load(tmp.path() / "sub" / "hp2pc.cfg");
  CHECK_EQ(back.get("mode"), std::string("portable"));
  CHECK_EQ(back.get("rom_source"), std::string("D:\\Games\\My ROM = here"));
  CHECK_EQ(back.get("multi"), std::string("line one line two"));
  CHECK_EQ(back.get("absent", "fallback"), std::string("fallback"));
  CHECK(!back.has("absent"));
  CHECK(KeyValueFile::load(tmp.path() / "missing.cfg").values().empty());
}

TEST(folder_name_rules) {
  CHECK(!why_bad_folder_name("game"));
  CHECK(!why_bad_folder_name("Hot Pursuit 2 (PS2)"));
  CHECK(why_bad_folder_name(""));
  CHECK(why_bad_folder_name(".."));
  CHECK(why_bad_folder_name("a\\b"));
  CHECK(why_bad_folder_name("a/b"));
  CHECK(why_bad_folder_name("what?"));
  CHECK(why_bad_folder_name("trailing."));
  CHECK(why_bad_folder_name("NUL"));
  CHECK(why_bad_folder_name("com1.txt"));
  CHECK(why_bad_folder_name(std::string(65, 'a')));
}

TEST(enum_text_roundtrips) {
  CHECK(parse_mode(to_string(Mode::Installed)) == Mode::Installed);
  CHECK(parse_mode(to_string(Mode::Portable)) == Mode::Portable);
  CHECK(parse_rom_storage(to_string(RomStorage::Reference)) == RomStorage::Reference);
  CHECK(!parse_mode("x"));
  CHECK(!parse_rom_storage(""));
}

// --------------------------------------------------------------------- convert

TEST(convert_portable_creates_the_whole_layout_inside_one_folder) {
  FakeDisc d;
  const fs::path base = d.tmp.path() / "converter dir";
  const fs::path game = base / "game";
  const Snapshot rom_before = snapshot(d.rom);

  std::vector<std::string> lines;
  std::map<std::string, int> stages_seen;
  ConvertEvents ev;
  ev.log = [&](std::string_view l) { lines.emplace_back(l); };
  ev.progress = [&](const Progress& p) {
    ++stages_seen[std::string(p.stage)];
    CHECK(p.done <= p.total);
  };
  const ConvertResult r = convert(d.request(game), ev, d.profile);
  CHECK(r.ok);
  CHECK(r.error.empty());

  // C6: nothing next to the game folder, everything inside it
  std::size_t siblings = 0;
  for (const auto& de : fs::directory_iterator(base)) {
    ++siblings;
    CHECK(de.path() == game);
  }
  CHECK_EQ(siblings, static_cast<std::size_t>(1));
  CHECK(fs::is_regular_file(game / "hp2pc.cfg"));
  CHECK(fs::is_directory(game / "logs"));
  CHECK(fs::is_regular_file(r.log_file));
  // ROM copy is byte-identical
  for (const RomManifestEntry& e : d.profile.files)
    CHECK(testkit::read_bytes(game / "rom" / e.path) == testkit::read_bytes(d.rom / e.path));
  // assets unpacked with names
  CHECK(fs::is_regular_file(game / "assets" / "manifest.tsv"));
  CHECK(testkit::read_bytes(game / "assets" / "GLOBAL" / "HUDTEX.BIN") == pattern(5000, 12));
  CHECK(testkit::read_bytes(game / "assets" / "NFS6.ICO") == pattern(700, 13));
  CHECK(fs::is_regular_file(game / "assets" / "_UNNAMED" / "DEADBEEF.BIN"));
  // C2: the ROM folder was only read
  CHECK(snapshot(d.rom) == rom_before);
  // progress was visible for every stage, and the log says what happened
  CHECK(stages_seen["validate"] > 0);
  CHECK(stages_seen["rom"] > 0);
  CHECK(stages_seen["unpack"] > 0);
  CHECK(contains(lines, "ROM stored: 6 file(s) copied and verified"));
  CHECK(contains(lines, "Assets unpacked: 4 file(s) written"));
  CHECK(contains(lines, "not produced by this converter version"));

  // state
  const KeyValueFile cfg = KeyValueFile::load(game / "hp2pc.cfg");
  CHECK_EQ(cfg.get("format"), std::string("1"));
  CHECK_EQ(cfg.get("mode"), std::string("portable"));
  CHECK_EQ(cfg.get("rom_storage"), std::string("copy"));
  CHECK(cfg.get("stage.unpack").starts_with("done"));
  REQUIRE(r.status.stages.size() == 4);
  CHECK(r.status.is_game_folder);
  CHECK(r.status.stages[0].state == StageState::Done);
  CHECK(r.status.stages[1].state == StageState::Done);
  CHECK(r.status.stages[2].state == StageState::Done);
  CHECK(r.status.stages[3].state == StageState::NotAvailable);
}

TEST(convert_rerun_detects_finished_work_and_rewrites_nothing) {
  FakeDisc d;
  const fs::path game = d.tmp.path() / "game";
  REQUIRE(convert(d.request(game), {}, d.profile).ok);
  Snapshot before = snapshot(game);

  std::vector<std::string> lines;
  ConvertEvents ev;
  ev.log = [&](std::string_view l) { lines.emplace_back(l); };
  const ConvertResult r = convert(d.request(game), ev, d.profile);
  CHECK(r.ok);
  CHECK(contains(lines, "Existing conversion found"));
  CHECK(contains(lines, "0 file(s) copied and verified by SHA-256, 6 already present"));
  CHECK(contains(lines, "0 file(s) written, 4 already present"));

  // Only bookkeeping changed: state file, the assets manifest (rewritten with
  // identical content) and one additional log.
  Snapshot after = snapshot(game);
  std::size_t new_logs = 0;
  for (const auto& [name, meta] : after) {
    if (name == "hp2pc.cfg" || name == "assets/manifest.tsv") continue;
    if (name.starts_with("logs/")) {
      if (!before.count(name)) ++new_logs;
      continue;
    }
    REQUIRE(before.count(name) == 1);
    CHECK(before[name] == meta);
  }
  CHECK(new_logs <= 1);  // same-second runs share one log file (appended)
  CHECK_EQ(after.size(), before.size() + new_logs);
}

TEST(convert_resumes_after_cancellation) {
  FakeDisc d;
  const fs::path game = d.tmp.path() / "game";
  int calls = 0;
  ConvertEvents ev;
  ev.cancelled = [&] { return ++calls > 6; };  // stop somewhere in the middle
  const ConvertResult r1 = convert(d.request(game), ev, d.profile);
  CHECK(!r1.ok);
  CHECK(r1.cancelled);
  CHECK(r1.error.find("resume") != std::string::npos);
  // no half-written files under final names, no stray .part files
  for (const auto& de : fs::recursive_directory_iterator(game)) CHECK(de.path().extension() != ".part");

  const ConvertResult r2 = convert(d.request(game), {}, d.profile);
  CHECK(r2.ok);
  for (const RomManifestEntry& e : d.profile.files)
    CHECK(testkit::read_bytes(game / "rom" / e.path) == testkit::read_bytes(d.rom / e.path));
  CHECK(fs::is_regular_file(game / "assets" / "manifest.tsv"));
}

TEST(convert_refuses_foreign_folders_and_the_rom_folder) {
  FakeDisc d;
  // a folder with someone else's files
  const fs::path foreign = d.tmp.path() / "Documents";
  testkit::write_bytes(foreign / "thesis.docx", pattern(100, 1));
  const Snapshot before = snapshot(foreign);
  ConvertResult r = convert(d.request(foreign), {}, d.profile);
  CHECK(!r.ok);
  CHECK(r.error.find("not a HotPursuit2PC game folder") != std::string::npos);
  CHECK(snapshot(foreign) == before);

  // inside the ROM folder (C2)
  const Snapshot rom_before = snapshot(d.rom);
  r = convert(d.request(d.rom / "game"), {}, d.profile);
  CHECK(!r.ok);
  CHECK(r.error.find("never written to") != std::string::npos);
  r = convert(d.request(d.rom), {}, d.profile);
  CHECK(!r.ok);
  CHECK(snapshot(d.rom) == rom_before);
  CHECK(!fs::exists(d.rom / "game"));

  // an empty existing folder is fine
  fs::create_directories(d.tmp.path() / "empty");
  CHECK(convert(d.request(d.tmp.path() / "empty"), {}, d.profile).ok);
}

TEST(convert_stops_on_a_bad_rom_before_creating_anything) {
  FakeDisc d;
  std::error_code ec;
  fs::rename(d.rom / "SLUS_203.62", d.tmp.path() / "gone", ec);
  const fs::path game = d.tmp.path() / "game";
  const ConvertResult r = convert(d.request(game), {}, d.profile);
  CHECK(!r.ok);
  CHECK_EQ(r.failed_stage, std::string("validate"));
  CHECK(r.error.find("SLUS_203.62 is missing") != std::string::npos);
  CHECK(!fs::exists(game));
}

TEST(convert_modes_and_rom_storage) {
  FakeDisc d;
  // portable + reference contradicts "self-contained"
  ConvertRequest req = d.request(d.tmp.path() / "p");
  req.rom_storage = RomStorage::Reference;
  ConvertResult r = convert(req, {}, d.profile);
  CHECK(!r.ok);
  CHECK(r.error.find("portable mode keeps a copy") != std::string::npos);
  CHECK(!fs::exists(d.tmp.path() / "p"));

  // installed + reference: no ROM copy, assets unpacked from the original
  req = d.request(d.tmp.path() / "installed here");
  req.mode = Mode::Installed;
  req.rom_storage = RomStorage::Reference;
  r = convert(req, {}, d.profile);
  CHECK(r.ok);
  CHECK(fs::is_regular_file(req.game_dir / "rom" / "rom-reference.tsv"));
  CHECK(!fs::exists(req.game_dir / "rom" / "SLUS_203.62"));
  CHECK(testkit::read_bytes(req.game_dir / "assets" / "NFS6.ICO") == pattern(700, 13));
  CHECK(r.status.mode == Mode::Installed);
  CHECK(r.status.rom_storage == RomStorage::Reference);
  CHECK(r.status.stages[1].state == StageState::Done);

  // resuming with a different storage setting is refused, not silently changed
  req.rom_storage = RomStorage::Copy;
  r = convert(req, {}, d.profile);
  CHECK(!r.ok);
  CHECK(r.error.find("created with ROM storage 'reference'") != std::string::npos);

  // --no-assets
  req = d.request(d.tmp.path() / "noassets");
  req.unpack_assets = false;
  r = convert(req, {}, d.profile);
  CHECK(r.ok);
  CHECK(!fs::exists(req.game_dir / "assets" / "manifest.tsv"));
}

TEST(convert_never_overwrites_a_mismatching_rom_copy_silently) {
  FakeDisc d;
  const fs::path game = d.tmp.path() / "game";
  REQUIRE(convert(d.request(game), {}, d.profile).ok);
  const fs::path victim = game / "rom" / "SLUS_203.62";
  testkit::write_bytes(victim, bytes_of("tampered"));

  ConvertResult r = convert(d.request(game), {}, d.profile);
  CHECK(!r.ok);
  CHECK_EQ(r.failed_stage, std::string("rom"));
  CHECK(r.error.find("Nothing was overwritten") != std::string::npos);
  CHECK(testkit::read_bytes(victim) == bytes_of("tampered"));
  CHECK(!r.status.last_error.empty());
  CHECK(r.status.stages[1].state == StageState::Partial);

  ConvertRequest repair = d.request(game);
  repair.repair = true;
  r = convert(repair, {}, d.profile);
  CHECK(r.ok);
  CHECK(testkit::read_bytes(victim) == pattern(4000, 11));
  CHECK(testkit::read_bytes(game / "rom" / "SLUS_203.62.mismatch-1") == bytes_of("tampered"));
  CHECK(r.status.last_error.empty());
}

TEST(portable_folder_is_relocatable_and_works_without_the_original_rom) {
  FakeDisc d;
  const fs::path game = d.tmp.path() / "first place" / "game";
  REQUIRE(convert(d.request(game), {}, d.profile).ok);

  // Move the game folder elsewhere and take the original ROM away.
  const fs::path moved = d.tmp.path() / "usb stick" / "hp2";
  fs::create_directories(moved.parent_path());
  fs::rename(game, moved);
  fs::rename(d.rom, d.tmp.path() / "rom is gone");

  std::vector<std::string> lines;
  ConvertEvents ev;
  ev.log = [&](std::string_view l) { lines.emplace_back(l); };
  ConvertRequest req = d.request(moved);  // still names the old ROM path
  ConvertResult r = convert(req, ev, d.profile);
  CHECK(r.ok);
  CHECK(contains(lines, "is not available; using the copy inside the game folder"));
  CHECK(contains(lines, "0 file(s) written, 4 already present"));
  CHECK(r.status.stages[1].state == StageState::Done);
  CHECK(r.status.stages[2].state == StageState::Done);

  // No ROM folder given at all: same thing.
  req.rom_dir.clear();
  r = convert(req, {}, d.profile);
  CHECK(r.ok);
  // The record of where the ROM originally came from is kept.
  CHECK(KeyValueFile::load(moved / "hp2pc.cfg").get("rom_source").find("Fake Disc (USA)") != std::string::npos);

  // A *new* folder without any ROM still fails clearly.
  req.game_dir = d.tmp.path() / "new";
  r = convert(req, {}, d.profile);
  CHECK(!r.ok);
  CHECK(r.error.find("No ROM folder was given") != std::string::npos);
}

TEST(inspect_and_find_game_folders) {
  FakeDisc d;
  const fs::path base = d.tmp.path() / "base";
  // nothing yet
  GameFolderStatus st = inspect_game_folder(base / "game", d.profile);
  CHECK(!st.exists);
  CHECK(!st.anything_done());
  CHECK(!find_game_folder(base));
  // foreign
  testkit::write_bytes(base / "stuff" / "a.txt", pattern(4, 1));
  st = inspect_game_folder(base / "stuff", d.profile);
  CHECK(st.exists);
  CHECK(st.foreign_content);
  CHECK(st.describe().find("NOT a HotPursuit2PC game folder") != std::string::npos);
  // ours, under a custom name; "game" wins when both exist
  REQUIRE(convert(d.request(base / "my hp2"), {}, d.profile).ok);
  REQUIRE(find_game_folder(base).has_value());
  CHECK(find_game_folder(base)->filename() == "my hp2");
  REQUIRE(convert(d.request(base / "game"), {}, d.profile).ok);
  CHECK(find_game_folder(base)->filename() == "game");
  st = inspect_game_folder(base / "game", d.profile);
  CHECK(st.is_game_folder);
  CHECK(st.anything_done());
  CHECK(st.describe().find("6 of 6 files present in rom/") != std::string::npos);
  CHECK(!st.rom_source.empty());
}

MINITEST_MAIN()
