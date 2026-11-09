//-------------------------------------------------------------------------------------------------
// EditorPlaytest.cpp -- see EditorPlaytest.h for the contract + the shadow-save compromise.
//
// Wraps EditorTaskRunner (process + combined stdout/stderr pipe + exit code) with the
// playtest-specific glue: save the mission in place, resolve/validate the game exe,
// stale-deploy mtime warning, launch with `-mission <stem>` (cwd = exe dir), and on exit
// archive the full captured log to <missionDir>/playtest-logs/<timestamp>.log.
//
// STEM + COPY BRIDGE CONTRACT (the load-bearing detail):
//   mc2.exe `-mission` takes a bare STEM, NOT a path.  The engine resolves the file as
//   `missionPath + stem + ".fit"/".pak"` where missionPath comes from system.cfg and is
//   effectively `data\missions\` relative to the game's cwd (mission.cpp:1500/1904/2166).
//   Passing a full saved path produced the mangled `data/missions/<abspath>.fit`, a missing
//   .fit, and the "Could not find Teams Block" fail-fast (mission.cpp:1985 -> exit 0xC0000409).
//   So we (1) derive the stem from the saved pak filename, (2) COPY both `<stem>.pak` and the
//   sibling `<stem>.fit` (they are saved next to each other) into the GAME install's
//   `<exeDir>\data\missions\` -- the game install may differ from the editor's, which is the
//   whole point of the bridge -- and (3) launch `-mission <stem>` with cwd = exeDir.
//
// PLAYTEST SAFETY (added 2026-06-11, torrin in-place-save incident):
//   The Slice-1 compromise saves the mission IN PLACE.  This destroyed an original campaign
//   mission (torrin.pak/.fit) when its content was dropped on re-save (the .old backup chain
//   was overwritten by repeat saves).  Until the real fix (shadow save, future slice) lands,
//   Start() guards the original three ways:
//     (1) PRISTINE SNAPSHOT -- one-time, never-overwritten copy of the pre-first-playtest
//         `<stem>.pak`+`.fit` to `<stem>.playtest-orig.pak`+`.fit`.  If the snapshot copy
//         fails the playtest ABORTS (we refuse to risk the in-place save with no backup).
//     (2) SAVE-SHRINK TRIPWIRE -- after the in-place save, if the new .fit is < 60% of the
//         snapshot .fit it loudly warns "save dropped content?" (non-blocking).
//     (3) CRASH-LOG GUARANTEE -- OnFinished archives on BOTH success and failure exits, and
//         appends the exit code into the archived log itself.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"   // MFC / <windows.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/gate_name.h"

#include "EditorPlaytest.h"
#include "EditorPlaytestResults.h"
#include "EditorTaskRunner.h"
#include "EditorData.h"
#include "EditorModProject.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace EditorPlaytest
{
namespace {

State        s_state = State::Idle;
EditorTaskRunner::TaskId s_task = EditorTaskRunner::kInvalidTask;
char         s_status[512] = "Idle -- load + save a mission, then Playtest.";
char         s_exePath[512] = "";
bool         s_exeResolved = false;

// Mod context captured at Start() time, consumed by the completion callbacks so the log
// is archived under the mod's .modproject tree (set only when the mission lives in a mod).
bool         s_modActive = false;
std::string  s_modRoot;      // `...\mods\<id>` absolute dir
std::string  s_modId;
std::string  s_runStamp;     // shared timestamp for this run's archive subdir

// Completion record for the headless `-playtest` CLI smoke (read via accessors
// after IsIdle() goes true). Written on the main thread by OnFinished.
int          s_lastExitCode = -1;
std::string  s_lastLogPath;

// Smoke-child mode (set by SetSmokeChildEnv before Start()): when true the
// launched game gets MC2_SMOKE_MODE=1 + `--mission <stem> --duration <sec>` so
// it auto-quits. Interactive playtest never sets this.
bool         s_smokeChild        = false;
int          s_smokeChildSeconds = 30;

// --- Runtime bridge v0 (mover.state) ---------------------------------------------------
// Records parsed from the running child's [MOVER v1] stdout lines. Each burst is
// bracketed by "[MOVER v1 begin] ..." then N "[MOVER v1] id=..." lines; on begin we
// reset the accumulator, then publish it as s_liveMovers when the next burst begins (or
// on the first record after a begin). Simpler/robust: accumulate into s_burstMovers and
// publish on EACH begin (so the panel sees the PREVIOUS complete burst). Main-thread only
// (driven by EditorTaskRunner::PumpMainThread via onLineMainThread).
std::vector<EditorPlaytest::MoverSnapshot> s_liveMovers;    // published (last complete burst)
std::vector<EditorPlaytest::MoverSnapshot> s_burstMovers;   // accumulating current burst
unsigned long s_liveMoversStamp = 0;

// --- helpers --------------------------------------------------------------------------

// Forward decls (definitions below; ArchiveLog uses these before their point of def).
std::string TimeStamp();
void MakeDirTree(const std::string& dir);

// Split a full file path into directory + filename-base (no extension).
void SplitPath(const std::string& full, std::string& dir, std::string& base)
{
	size_t slash = full.find_last_of("/\\");
	dir  = (slash != std::string::npos) ? full.substr(0, slash) : std::string(".");
	std::string fileNm = (slash != std::string::npos) ? full.substr(slash + 1) : full;
	size_t dot = fileNm.find_last_of('.');
	base = (dot != std::string::npos) ? fileNm.substr(0, dot) : fileNm;
}

bool FileExists(const char* p)
{
	DWORD a = GetFileAttributesA(p);
	return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// File size in bytes, or 0 if the file is missing/unreadable (used by the shrink tripwire,
// where 0 means "no comparison possible" -> tripwire silently skips).
unsigned long long FileSizeBytes(const char* p)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fad))
		return 0;
	ULARGE_INTEGER sz;
	sz.LowPart  = fad.nFileSizeLow;
	sz.HighPart = fad.nFileSizeHigh;
	return sz.QuadPart;
}

// Normalize a (possibly relative / `..`-laden) path to a full absolute path.
// On failure returns the input unchanged so callers still have something usable.
std::string AbsPath(const std::string& in)
{
	char buf[1024];
	DWORD n = GetFullPathNameA(in.c_str(), sizeof(buf), buf, nullptr);
	if (n > 0 && n < sizeof(buf))
		return std::string(buf);
	return in;
}

// Case-insensitive path equality (after normalization the caller should already
// have run AbsPath on both sides; Windows paths are case-insensitive).
bool SamePath(const std::string& a, const std::string& b)
{
	return _stricmp(a.c_str(), b.c_str()) == 0;
}

// Returns true if the exe was last written more than 24h ago (stale-deploy guard).
bool ExeIsStale(const char* p)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fad))
		return false;
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER w, n;
	w.LowPart = fad.ftLastWriteTime.dwLowDateTime;  w.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	n.LowPart = now.dwLowDateTime;                  n.HighPart = now.dwHighDateTime;
	const ULONGLONG kDay = 24ULL * 60ULL * 60ULL * 10000000ULL;   // 100ns ticks / day
	return (n.QuadPart > w.QuadPart) && ((n.QuadPart - w.QuadPart) > kDay);
}

// Resolve the game exe once: env MC2_PLAYTEST_EXE, then probe candidates relative to the
// editor cwd.  Stores into s_exePath.  Idempotent; safe to call repeatedly.
void ResolveExe()
{
	if (s_exeResolved && s_exePath[0])
		return;

	const std::string envExe = mc2::gate::str<"MC2_PLAYTEST_EXE">();   // "" when unset or empty
	if (!envExe.empty() && envExe.size() < 512)
	{
		std::string abs = AbsPath(envExe.c_str());
		strncpy(s_exePath, abs.c_str(), sizeof(s_exePath) - 1);
		s_exePath[sizeof(s_exePath) - 1] = 0;
		s_exeResolved = true;
		return;
	}

	// Probe candidates relative to the editor's working directory.
	const char* candidates[] = {
		"..\\mc2-win64-v0.4\\mc2.exe",
		".\\mc2.exe",
		"mc2.exe",
	};
	for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i)
	{
		if (FileExists(candidates[i]))
		{
			std::string abs = AbsPath(candidates[i]);
			strncpy(s_exePath, abs.c_str(), sizeof(s_exePath) - 1);
			s_exePath[sizeof(s_exePath) - 1] = 0;
			s_exeResolved = true;
			return;
		}
	}

	// Nothing found: leave the default probe path so the user can see/edit it, but do NOT
	// latch s_exeResolved -- the exe may be deployed between now and the next Start(), so a
	// later call must re-probe instead of being short-circuited by the early-return above.
	if (!s_exePath[0])
	{
		strncpy(s_exePath, candidates[0], sizeof(s_exePath) - 1);
		s_exePath[sizeof(s_exePath) - 1] = 0;
	}
}

// Archive the captured log next to the mission.  Returns the written path (empty on fail).
// `prefix` is prepended to the timestamped filename (e.g. "stopped-") to distinguish a
// user-terminated run from a natural exit.
std::string ArchiveLog(const std::string& missionPak, const std::string& log,
	const char* prefix = "")
{
	std::string logDir;
	std::string fileName;

	if (s_modActive)
	{
		// Mod-aware: <modRoot>\.modproject\playtest\<stamp>\<prefix>playtest.log
		std::string stamp = s_runStamp.empty() ? TimeStamp() : s_runStamp;
		logDir = s_modRoot + "\\.modproject\\playtest\\" + stamp;
		MakeDirTree(logDir);
		if (GetFileAttributesA(logDir.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			snprintf(s_status, sizeof(s_status),
				"MakeDirTree failed for log dir (%s), err %lu -- log archive will fail.",
				logDir.c_str(), GetLastError());
		}
		fileName = std::string(prefix ? prefix : "") + "playtest.log";
	}
	else
	{
		// Legacy: <missionDir>\playtest-logs\<prefix><stamp>.log
		std::string dir, base;
		SplitPath(missionPak, dir, base);
		logDir = dir + "/playtest-logs";
		CreateDirectoryA(logDir.c_str(), nullptr);   // ignore "already exists"
		fileName = std::string(prefix ? prefix : "") + TimeStamp() + ".log";
	}

	std::string logPath = logDir + "/" + fileName;
	FILE* f = nullptr;
	if (fopen_s(&f, logPath.c_str(), "wb") == 0 && f)
	{
		fwrite(log.data(), 1, log.size(), f);
		fclose(f);
		return logPath;
	}
	return std::string();
}

// Format the current local time as yyyymmdd-hhmmss.
std::string TimeStamp()
{
	time_t t = time(nullptr);
	struct tm lt;
	localtime_s(&lt, &t);
	char stamp[32];
	snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
		lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
	return std::string(stamp);
}

// Create a directory tree (mkdir -p) for an absolute path.  Single-level
// CreateDirectoryA only, so walk each separator and create as we go.
void MakeDirTree(const std::string& dir)
{
	std::string acc;
	for (size_t i = 0; i < dir.size(); ++i)
	{
		char c = dir[i];
		acc.push_back(c);
		if (c == '\\' || c == '/')
			CreateDirectoryA(acc.c_str(), nullptr);
	}
	CreateDirectoryA(acc.c_str(), nullptr);
}

// Returns the last-write FILETIME of a file as a 64-bit value, or 0 if missing.
unsigned long long FileMTime(const char* p)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fad))
		return 0;
	ULARGE_INTEGER t;
	t.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
	t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	return t.QuadPart;
}

// Incremental file copy: copy src -> dst only when dst is missing or src is strictly newer.
// Returns: 1 = copied, 0 = skipped (already up to date), -1 = copy FAILED.
int CopyFileIncremental(const std::string& src, const std::string& dst)
{
	unsigned long long sM = FileMTime(src.c_str());
	unsigned long long dM = FileMTime(dst.c_str());
	const bool dstExists = (dM != 0) || FileExists(dst.c_str());
	if (dstExists && sM != 0 && sM <= dM)
		return 0;   // up to date -- skip
	if (SamePath(AbsPath(src), AbsPath(dst)))
		return 0;   // same file (CopyFile onto itself fails err 32) -- nothing to do
	if (!CopyFileA(src.c_str(), dst.c_str(), FALSE))
		return -1;
	return 1;
}

// Recursively mirror srcDir -> dstDir (incremental).  Creates dstDir as needed.  Skips any
// component named ".modproject" (editor-only project state, must not ship into the game).
// On success returns true and adds the count of files actually copied to *copied.  On the
// FIRST copy failure sets *failSrc/*failDst and returns false (caller aborts the launch).
bool MirrorTreeIncremental(const std::string& srcDir, const std::string& dstDir,
	int* copied, std::string* failSrc, std::string* failDst)
{
	MakeDirTree(dstDir);

	WIN32_FIND_DATAA fd;
	std::string pattern = srcDir + "\\*";
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return true;   // empty/absent dir -- nothing to mirror (not an error)

	bool ok = true;
	do {
		const char* nm = fd.cFileName;
		if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
			continue;
		std::string srcChild = srcDir + "\\" + nm;
		std::string dstChild = dstDir + "\\" + nm;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (_stricmp(nm, ".modproject") == 0)
				continue;   // never copy editor project state into the game
			if (!MirrorTreeIncremental(srcChild, dstChild, copied, failSrc, failDst))
			{
				ok = false;
				break;
			}
		}
		else
		{
			int r = CopyFileIncremental(srcChild, dstChild);
			if (r < 0)
			{
				if (failSrc) *failSrc = srcChild;
				if (failDst) *failDst = dstChild;
				ok = false;
				break;
			}
			if (r > 0 && copied)
				++(*copied);
		}
	} while (FindNextFileA(h, &fd));

	FindClose(h);
	return ok;
}

// --- mod-root detection ----------------------------------------------------------------
// Walk the saved mission's absolute path upward looking for a `...\mods\<id>\...` layout
// (case-insensitive match on a "mods" path component).  On success fills modRoot (the
// `...\mods\<id>` absolute dir) + modId and returns true; else returns false (no mod).
bool DetectModRoot(const std::string& missionAbs, std::string& modRoot, std::string& modId)
{
	// Tokenize on both separators, remembering each token's end offset so we can rebuild
	// the absolute prefix up to and including the <id> component.
	std::vector<std::pair<std::string, size_t>> comps;   // (lowercased token, endOffset)
	size_t start = 0;
	for (size_t i = 0; i <= missionAbs.size(); ++i)
	{
		if (i == missionAbs.size() || missionAbs[i] == '\\' || missionAbs[i] == '/')
		{
			if (i > start)
			{
				std::string tok = missionAbs.substr(start, i - start);
				std::string low = tok;
				for (char& c : low) c = (char)tolower((unsigned char)c);
				comps.push_back(std::make_pair(low, i));
			}
			start = i + 1;
		}
	}

	// Find a "mods" component that has at least one component after it (the <id>).
	for (size_t i = 0; i + 1 < comps.size(); ++i)
	{
		if (comps[i].first == "mods")
		{
			// <id> = the original-case token right after "mods".
			size_t idEnd = comps[i + 1].second;
			modRoot = missionAbs.substr(0, idEnd);   // `...\mods\<id>`
			// Recover original-case id from modRoot's last component.
			size_t slash = modRoot.find_last_of("/\\");
			modId = (slash != std::string::npos) ? modRoot.substr(slash + 1) : modRoot;
			return true;
		}
	}
	return false;
}

// Pull the last non-empty line of the captured log for the status string.
std::string LastLine(const std::string& log)
{
	size_t end = log.find_last_not_of("\r\n");
	if (end == std::string::npos) return std::string();
	size_t start = log.find_last_of('\n', end);
	start = (start == std::string::npos) ? 0 : start + 1;
	return log.substr(start, end - start + 1);
}

// --- [MOVER vN] stdout parser (runtime bridge) -----------------------------------------
// Tolerant key=value scanner over a single output line. Version-agnostic: matches any
// "[MOVER v<N>" tag so a v1-only game exe (no brain/path fields) and the current v2 stream
// both parse. Recognizes the burst delimiter "[MOVER v<N> begin]" (publishes the prior
// complete burst, then resets) and per-mover lines:
//   "[MOVER v2] id=.. name=.. team=.. pos=x,y,z hp=.. pilot=.. order=val/name target=.. \
//    brain=.. mstate=.. pgoal=x,y,z pstep=cur/total pcost=.. tgtpos=x,y,z"
// Keys absent in older streams keep the MoverSnapshot defaults. Unknown keys ignored.
// Runs on the MAIN thread.
void BridgeParseLine(const std::string& line)
{
	// Any-version tag match. "[MOVER v" then digits, then either " begin]" or "] payload".
	if (line.compare(0, 8, "[MOVER v") != 0)
		return;
	size_t close = line.find(']');
	if (close == std::string::npos)
		return;

	// Burst boundary: publish what we accumulated, start fresh. The token before ']' is
	// " begin" for delimiters.
	if (line.find(" begin]") != std::string::npos)
	{
		s_liveMovers      = s_burstMovers;
		s_liveMoversStamp = GetTickCount();
		s_burstMovers.clear();
		return;
	}

	EditorPlaytest::MoverSnapshot m;
	// Walk space-separated key=value tokens after the "] ". name/pilot are single tokens.
	const char* p = line.c_str() + close + 1;
	while (*p == ' ') ++p;
	while (*p)
	{
		while (*p == ' ') ++p;
		if (!*p) break;
		const char* keyStart = p;
		while (*p && *p != '=' && *p != ' ') ++p;
		std::string key(keyStart, p);
		if (*p != '=') { while (*p && *p != ' ') ++p; continue; }
		++p; // skip '='
		const char* valStart = p;
		while (*p && *p != ' ') ++p;
		std::string val(valStart, p);

		if      (key == "id")    m.id   = atol(val.c_str());
		else if (key == "name")  m.name = val;
		else if (key == "team")  m.team = atol(val.c_str());
		else if (key == "hp")    m.hp   = (float)atof(val.c_str());
		else if (key == "pilot") m.pilot = val;
		else if (key == "target")m.target = atol(val.c_str());
		else if (key == "pos")
		{
			float x = 0, y = 0, z = 0;
			sscanf_s(val.c_str(), "%f,%f,%f", &x, &y, &z);
			m.x = x; m.y = y; m.z = z;
		}
		else if (key == "order")
		{
			// "val/name"
			m.orderVal = atol(val.c_str());
			size_t slash = val.find('/');
			m.orderName = (slash != std::string::npos) ? val.substr(slash + 1) : val;
		}
		// --- v2 fields ---
		else if (key == "brain")  m.brain     = atol(val.c_str());
		else if (key == "mstate") m.moveState = atol(val.c_str());
		else if (key == "pgoal")
		{
			float x = 0, y = 0, z = 0;
			sscanf_s(val.c_str(), "%f,%f,%f", &x, &y, &z);
			m.gx = x; m.gy = y; m.gz = z;
		}
		else if (key == "pstep")
		{
			// "cur/total"
			m.pathStep = atol(val.c_str());
			size_t slash = val.find('/');
			m.pathSteps = (slash != std::string::npos) ? atol(val.c_str() + slash + 1) : 0;
		}
		else if (key == "pcost")  m.pathCost = atol(val.c_str());
		else if (key == "tgtpos")
		{
			float x = 0, y = 0, z = 0;
			sscanf_s(val.c_str(), "%f,%f,%f", &x, &y, &z);
			m.tx = x; m.ty = y; m.tz = z;
		}
	}
	if (m.id != -1)
		s_burstMovers.push_back(m);
}

// Shared completion handler (success or failure both archive the log + report exit code).
void OnFinished(const EditorTaskRunner::TaskResult& res)
{
	s_state = State::Idle;
	s_task  = EditorTaskRunner::kInvalidTask;

	const bool pass = (res.exitCode == 0);

	// CRASH-LOG GUARANTEE: archive on BOTH success and failure (onSuccess + onFailure both
	// route here).  Append the exit-code verdict INTO the archived log itself so a crash
	// (nonzero exit) is self-documenting even when read in isolation.
	std::string logBody = res.log;
	{
		char tail[96];
		snprintf(tail, sizeof(tail), "\n[playtest] exit code %d (%s)\n",
			res.exitCode, pass ? "PASS" : "FAIL");
		logBody += tail;
	}

	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : nullptr;
	std::string logPath = (missionPath && missionPath[0])
		? ArchiveLog(missionPath, logBody)
		: std::string();
	std::string last = LastLine(res.log);

	// Completion record for the headless `-playtest` CLI smoke accessors.
	s_lastExitCode = res.exitCode;
	s_lastLogPath  = logPath;

	// Playtest Results panel: parse the just-archived log (we have the exact body in hand,
	// so parse the text directly rather than re-reading the file we just wrote). Pure read;
	// no engine telemetry added. Skipped only when the archive write failed (no path).
	EditorPlaytestResults::SetResult(
		EditorPlaytestResults::ParseLogText(logBody,
			logPath.empty() ? nullptr : logPath.c_str()));

	char modTag[160] = "";
	if (s_modActive)
		snprintf(modTag, sizeof(modTag), "mod: %s  ", s_modId.c_str());

	snprintf(s_status, sizeof(s_status),
		"%s%s (exit %d). Log: %s%s%s",
		modTag,
		pass ? "PASS" : "FAIL",
		res.exitCode,
		logPath.empty() ? "(write failed)" : logPath.c_str(),
		last.empty() ? "" : "  | ",
		last.empty() ? "" : last.c_str());
}

void OnCancelled()
{
	s_state = State::Idle;
	s_task  = EditorTaskRunner::kInvalidTask;

	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : nullptr;
	(void)missionPath;
	snprintf(s_status, sizeof(s_status), "Stopped (playtest terminated).");
}

} // anonymous namespace

// --- public API -----------------------------------------------------------------------

bool IsRunning()
{
	return s_state == State::Running;
}

bool IsIdle()
{
	return s_state == State::Idle;
}

int LastExitCode()
{
	return s_lastExitCode;
}

const char* LastLogPath()
{
	return s_lastLogPath.c_str();
}

const char* LastModId()
{
	return s_modId.c_str();
}

const std::vector<MoverSnapshot>& LiveMovers()
{
	return s_liveMovers;
}

unsigned long LiveMoversStamp()
{
	return s_liveMoversStamp;
}

void SetSmokeChildEnv(bool enabled, int seconds)
{
	s_smokeChild        = enabled;
	if (seconds > 0)
		s_smokeChildSeconds = seconds;
}

bool CanPlaytest()
{
	if (IsRunning())
		return false;
	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : nullptr;
	return (missionPath && missionPath[0]);
}

void Start()
{
	if (IsRunning())
		return;

	// Clear the prior run's completion record so the `-playtest` CLI accessors do
	// not read a stale exit code/log path before this run finishes.
	s_lastExitCode = -1;
	s_lastLogPath.clear();

	// Reset runtime-bridge telemetry for the new run.
	s_liveMovers.clear();
	s_burstMovers.clear();
	s_liveMoversStamp = 0;

	if (!EditorData::instance)
	{
		snprintf(s_status, sizeof(s_status), "No editor data loaded.");
		return;
	}

	const char* missionPath = EditorData::instance->getMapName();
	if (!missionPath || !missionPath[0])
	{
		snprintf(s_status, sizeof(s_status), "No mission loaded -- save a mission first.");
		return;
	}

	// --- PRISTINE SNAPSHOT (playtest safety, torrin incident 2026-06-11) ----------------
	// BEFORE the in-place save can mutate anything, make a one-time never-overwritten copy of
	// the pre-first-playtest .pak + .fit.  If the snapshot exists already we keep it as-is
	// (preserves the true original across repeat playtests).  If we have an original to copy
	// but the copy FAILS, we ABORT -- never risk the destructive in-place save with no backup.
	std::string snapShrinkWarn;            // appended to Running status if the tripwire trips
	unsigned long long snapFitBytes = 0;   // original .fit size for the shrink tripwire
	bool snapTookThisRun = false;
	{
		std::string origDir, origBase;
		SplitPath(missionPath, origDir, origBase);
		std::string origPak  = origDir + "\\" + origBase + ".pak";
		std::string origFit  = origDir + "\\" + origBase + ".fit";
		std::string snapPak  = origDir + "\\" + origBase + ".playtest-orig.pak";
		std::string snapFit  = origDir + "\\" + origBase + ".playtest-orig.fit";

		if (!FileExists(snapPak.c_str()) || !FileExists(snapFit.c_str()))
		{
			// First playtest of this mission -- take the snapshot now (only the files that
			// actually exist; a brand-new unsaved mission may have neither yet).
			bool anyOrig = FileExists(origPak.c_str()) || FileExists(origFit.c_str());
			if (anyOrig)
			{
				if (FileExists(origPak.c_str()) &&
					!CopyFileA(origPak.c_str(), snapPak.c_str(), TRUE) &&
					GetLastError() != ERROR_FILE_EXISTS)
				{
					snprintf(s_status, sizeof(s_status),
						"Pristine snapshot FAILED (%s -> %s, err %lu) -- aborting playtest to "
						"protect the original.", origPak.c_str(), snapPak.c_str(), GetLastError());
					return;
				}
				if (FileExists(origFit.c_str()) &&
					!CopyFileA(origFit.c_str(), snapFit.c_str(), TRUE) &&
					GetLastError() != ERROR_FILE_EXISTS)
				{
					snprintf(s_status, sizeof(s_status),
						"Pristine snapshot FAILED (%s -> %s, err %lu) -- aborting playtest to "
						"protect the original.", origFit.c_str(), snapFit.c_str(), GetLastError());
					return;
				}
				snapTookThisRun = true;
			}
		}
		// Record snapshot .fit size for the post-save shrink tripwire (0 = skip comparison).
		snapFitBytes = FileSizeBytes(snapFit.c_str());
	}

	// COMPROMISE (see header): save IN PLACE.  EditorData::save() mutates path state
	// (setMapName) + writes .old backups; a non-mutating shadow copy to .playtest/ would
	// need a >50-line EditorData refactor -- out of slice budget.  We launch the saved pak.
	std::string savedPak = missionPath;
	if (!EditorData::instance->save(savedPak.c_str()))
	{
		snprintf(s_status, sizeof(s_status), "Mission save failed -- aborting playtest.");
		return;
	}
	// save() may rewrite mapName; re-read to launch exactly what was written.
	if (EditorData::instance->getMapName())
		savedPak = EditorData::instance->getMapName();

	// --- SAVE-SHRINK TRIPWIRE (playtest safety) ---------------------------------------
	// If the just-saved .fit is < 60% of the pristine snapshot .fit, the in-place save likely
	// dropped content (the torrin failure mode: mech spawns + buildings vanished).  Loud but
	// NON-blocking -- the snapshot still holds the original, so let the user see the run.
	if (snapFitBytes > 0)
	{
		std::string newDir, newBase;
		SplitPath(savedPak, newDir, newBase);
		std::string newFit = newDir + "\\" + newBase + ".fit";
		unsigned long long newFitBytes = FileSizeBytes(newFit.c_str());
		if (newFitBytes > 0 && newFitBytes * 100ULL < snapFitBytes * 60ULL)
		{
			char warn[256];
			snprintf(warn, sizeof(warn),
				"WARNING: save dropped content? fit shrank %llu->%llu bytes.\n",
				snapFitBytes, newFitBytes);
			snapShrinkWarn = warn;
		}
	}

	// --- mod-root detection -------------------------------------------------
	// Reset per-run mod context, then detect whether the saved mission lives inside a
	// `...\mods\<id>\...` layout.  When it does, the child gets MC2_ACTIVE_MOD + state
	// dump, and logs archive under the mod's .modproject tree.
	s_modActive = false;
	s_modRoot.clear();
	s_modId.clear();
	s_runStamp = TimeStamp();
	{
		// Prefer the active Mod Project's id/root (explicit binding wins over path sniffing);
		// fall back to path-derived `...\mods\<id>\...` detection when no project is open.
		if (EditorModProject::IsActive() && EditorModProject::Id()[0])
		{
			s_modId   = EditorModProject::Id();
			s_modRoot = EditorModProject::RootPath();
			s_modActive = true;
		}
		else
		{
			std::string missionAbs = AbsPath(savedPak);
			if (DetectModRoot(missionAbs, s_modRoot, s_modId) && !s_modId.empty())
				s_modActive = true;
		}
	}

	ResolveExe();
	if (!FileExists(s_exePath))
	{
		snprintf(s_status, sizeof(s_status),
			"Game exe not found: %s  (set MC2_PLAYTEST_EXE or edit the path).", s_exePath);
		return;
	}

	std::string exeDir, exeBase;
	SplitPath(s_exePath, exeDir, exeBase);

	// --- STEM + COPY BRIDGE (see file header) -----------------------------------------
	// The engine resolves `-mission <stem>` as data\missions\<stem>.fit/.pak relative to the
	// game's cwd (= exeDir).  Derive the stem and bridge BOTH the .pak and its sibling .fit
	// into the game install's data\missions\, since the game install may differ from ours.
	std::string srcDir, srcBase;
	SplitPath(savedPak, srcDir, srcBase);
	std::string stem = srcBase;

	std::string srcPak = srcDir + "\\" + stem + ".pak";
	std::string srcFit = srcDir + "\\" + stem + ".fit";
	if (!FileExists(srcPak.c_str()))
		srcPak = savedPak;   // fall back to whatever save() actually wrote
	if (!FileExists(srcFit.c_str()))
	{
		snprintf(s_status, sizeof(s_status),
			"Mission .fit not found next to pak (%s) -- cannot bridge to game; aborting.",
			srcFit.c_str());
		return;
	}

	// Bridge destination.  Default = the game's base mission dir.  For a MOD mission the
	// engine mounts mods\<id>\data\missions\ (via MC2_ACTIVE_MOD), so bridge there instead
	// -- BUT only if the detected mod root is NOT already under exeDir (i.e. an editor-side
	// mods folder distinct from the game install).  If the mod root IS under exeDir, the
	// engine's own mods\<id>\data\missions\ already holds the file (same-file skip handles it).
	std::string dstDir = exeDir + "\\data\\missions";
	std::string exeDirAbsLow = AbsPath(exeDir);
	for (char& c : exeDirAbsLow) c = (char)tolower((unsigned char)c);
	std::string modRootAbsLow = AbsPath(s_modRoot);
	for (char& c : modRootAbsLow) c = (char)tolower((unsigned char)c);
	// Require a real path-component boundary so "C:\games\mc2" does not spuriously prefix
	// "C:\games\mc2-mods\...": modRoot must be strictly longer than exeDir AND the next
	// char must be a path separator.
	bool modUnderExe = s_modActive &&
		modRootAbsLow.size() > exeDirAbsLow.size() &&
		modRootAbsLow.compare(0, exeDirAbsLow.size(), exeDirAbsLow) == 0 &&
		(modRootAbsLow[exeDirAbsLow.size()] == '\\' ||
		 modRootAbsLow[exeDirAbsLow.size()] == '/');

	std::string bridgeNote;          // appended to Running status (file-count breadcrumb)

	if (s_modActive && !modUnderExe)
	{
		// --- EDITOR-SIDE MOD: mirror the WHOLE mod data tree, not just pak+fit ---------
		// The half-empty-mod incident (DarkRain area16): copying only <stem>.pak/.fit left the
		// game mounting a mod missing its scripts (warriors/nop.abl, tgl stubs, ...) -> the
		// engine STOP()'d (a no-op in release) then NULL-deref-crashed.  Mirror the entire mod
		// `data\` tree into the game install's mods\<id>\data\ so MC2_ACTIVE_MOD sees a complete
		// mod.  Incremental (copy only when dst missing or src newer) so re-runs over a multi-MB
		// texture tree stay cheap.  On any copy failure: loud error + abort launch.
		std::string modDstRoot = exeDir + "\\mods\\" + s_modId;
		std::string srcDataDir = s_modRoot + "\\data";
		std::string dstDataDir = modDstRoot + "\\data";

		int copied = 0;
		std::string failSrc, failDst;
		if (!MirrorTreeIncremental(srcDataDir, dstDataDir, &copied, &failSrc, &failDst))
		{
			snprintf(s_status, sizeof(s_status),
				"Failed bridging mod data (%s -> %s), err %lu; aborting playtest.",
				failSrc.c_str(), failDst.c_str(), GetLastError());
			return;
		}

		// Also bridge the mod manifest (mod.json) when present -- the engine reads it to set up
		// the mount.  Missing manifest is not fatal (treat absent source as nothing to do).
		std::string srcModJson = s_modRoot + "\\mod.json";
		if (FileExists(srcModJson.c_str()))
		{
			MakeDirTree(modDstRoot);
			int r = CopyFileIncremental(srcModJson, modDstRoot + "\\mod.json");
			if (r < 0)
			{
				snprintf(s_status, sizeof(s_status),
					"Failed bridging mod.json into game (%s), err %lu; aborting playtest.",
					srcModJson.c_str(), GetLastError());
				return;
			}
			if (r > 0)
				++copied;
		}

		char nb[96];
		snprintf(nb, sizeof(nb), "bridged mod data (%d files copied)\n", copied);
		bridgeNote = nb;
	}
	else
	{
		// Non-mod mission (or a mod already living under the game install): just bridge the
		// single mission's pak + fit into the game's mission dir (the original behaviour).
		MakeDirTree(dstDir);

		// Normalize all four paths to absolute so the same-file check is reliable (the mission
		// may have been opened FROM the game install, making src == dst via a relative `..\`
		// dest) and so status/launch never leak confusing relative paths.
		srcPak = AbsPath(srcPak);
		srcFit = AbsPath(srcFit);
		std::string dstPak = AbsPath(dstDir + "\\" + stem + ".pak");
		std::string dstFit = AbsPath(dstDir + "\\" + stem + ".fit");

		// Per-file: if src and dst resolve to the same file, it is already in place -- skip the
		// copy silently (copying a file onto itself fails with sharing-violation err 32).
		if (!SamePath(srcPak, dstPak) && !CopyFileA(srcPak.c_str(), dstPak.c_str(), FALSE))
		{
			snprintf(s_status, sizeof(s_status),
				"Failed to copy pak into game (%s -> %s), err %lu; aborting.",
				srcPak.c_str(), dstPak.c_str(), GetLastError());
			return;
		}
		if (!SamePath(srcFit, dstFit) && !CopyFileA(srcFit.c_str(), dstFit.c_str(), FALSE))
		{
			snprintf(s_status, sizeof(s_status),
				"Failed to copy fit into game (%s -> %s), err %lu; aborting.",
				srcFit.c_str(), dstFit.c_str(), GetLastError());
			return;
		}
	}

	std::string stalePrefix;
	if (!bridgeNote.empty())
		stalePrefix += bridgeNote;
	if (snapTookThisRun)
		stalePrefix += "Pristine snapshot saved (.playtest-orig.pak/.fit) -- original preserved.\n";
	if (!snapShrinkWarn.empty())
		stalePrefix += snapShrinkWarn;
	if (ExeIsStale(s_exePath))
		stalePrefix += "WARNING: game exe is >24h old (possible stale deploy).\n";

	// Quote the stem only when it contains spaces (engine treats the token as the bare stem).
	bool stemHasSpace = (stem.find(' ') != std::string::npos);

	EditorTaskRunner::TaskSpec spec;
	spec.name = "Playtest";
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), stemHasSpace ? "\"%s\" -mission \"%s\"" : "\"%s\" -mission %s",
		s_exePath, stem.c_str());

	// SMOKE-CHILD MODE (headless `-playtest` CLI only): append gos_smoke's argv so
	// the game auto-quits after s_smokeChildSeconds. gos_smoke requires the
	// double-dash `--mission <stem>` form (separate from the engine's `-mission`
	// loader above) whenever MC2_SMOKE_MODE is set, plus `--duration <sec>`.
	if (s_smokeChild)
	{
		int secs = s_smokeChildSeconds;
		if (secs < 1)   secs = 1;
		if (secs > 600) secs = 600;
		char tail[256];
		snprintf(tail, sizeof(tail),
			stemHasSpace ? " --mission \"%s\" --duration %d" : " --mission %s --duration %d",
			stem.c_str(), secs);
		size_t cur = strlen(cmd);
		if (cur + strlen(tail) < sizeof(cmd))
			strcat(cmd, tail);

		// ACTIVE-SIM SMOKE (bridge fixtures): when MC2_SMOKE_ACTIVE=1 is set, append
		// gos_smoke's `--smoke-active` so the scenario clock advances and live movers
		// tick -- required for the [MOVER v1] runtime-bridge telemetry the --assert-bridge
		// harness verifies. Passive smoke (the default) is a frozen-scenario fly-through
		// and emits no mover state. This only forwards an existing engine flag; it adds
		// no new telemetry.
		if (mc2::gate::get<"MC2_SMOKE_ACTIVE">())
		{
			const char* act = " --smoke-active";
			if (strlen(cmd) + strlen(act) < sizeof(cmd))
				strcat(cmd, act);
		}
	}
	spec.commandLine     = cmd;
	spec.workingDirectory = exeDir;
	spec.onSuccessMainThread = &OnFinished;
	spec.onFailureMainThread = &OnFinished;
	spec.onCancelMainThread  = &OnCancelled;
	spec.onLineMainThread    = &BridgeParseLine;   // runtime bridge v0 (mover.state)

	// Runtime bridge v0: always ask the child to emit [MOVER v1] mover.state. The
	// emission is throttled game-side (~2s) and read-only, so the cost is negligible;
	// the editor consumes it for the Gameplay Debugger live table.
	spec.envExtra.push_back(std::make_pair(
		std::string(mc2::gate::name<"MC2_BRIDGE_MOVER_STATE">()), std::string("1")));

	// Mod-aware env: when the mission lives in a mod, mount it + ask for a state dump.
	if (s_modActive)
	{
		spec.envExtra.push_back(std::make_pair(std::string(mc2::gate::name<"MC2_ACTIVE_MOD">()), s_modId));
		spec.envExtra.push_back(std::make_pair(std::string(mc2::gate::name<"MC2_DEBUG_STATE_DUMP">()), std::string("1")));
	}

	// SMOKE-CHILD env: gos_smoke enables itself when MC2_SMOKE_MODE is present.
	// Paired with the `--mission/--duration` argv appended above so the child
	// auto-quits cleanly (exit 0) instead of running until the timeout.
	if (s_smokeChild)
		spec.envExtra.push_back(std::make_pair(std::string(mc2::gate::name<"MC2_SMOKE_MODE">()), std::string("1")));

	s_task = EditorTaskRunner::StartTask(spec);
	if (s_task == EditorTaskRunner::kInvalidTask)
	{
		s_state = State::Idle;
		snprintf(s_status, sizeof(s_status), "Failed to launch game process.");
		return;
	}

	s_state = State::Running;
	if (s_modActive)
		snprintf(s_status, sizeof(s_status),
			"%sRunning: mod: %s  -mission %s  (bridged to %s)",
			stalePrefix.c_str(), s_modId.c_str(), stem.c_str(), dstDir.c_str());
	else
		snprintf(s_status, sizeof(s_status), "%sRunning: -mission %s  (bridged to %s)",
			stalePrefix.c_str(), stem.c_str(), dstDir.c_str());
}

void Stop()
{
	if (!IsRunning() || s_task == EditorTaskRunner::kInvalidTask)
		return;

	// The worker's captured process log is private to EditorTaskRunner and there is no
	// public accessor (and we must not modify EditorTaskRunner here), so the natural-exit
	// archive in OnFinished cannot run for a cancelled task -- PumpMainThread routes cancels
	// to OnCancelMainThread (which carries no TaskResult/log).  Archive a "stopped-" marker
	// note now, BEFORE CancelTask, so a user-terminated run still leaves a breadcrumb next
	// to the mission instead of dropping the record entirely.
	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : nullptr;
	if (missionPath && missionPath[0])
	{
		std::string note = "Playtest STOPPED by user (process terminated).\n";
		note += "Status at stop: ";
		note += s_status;
		note += "\n(Full process stdout/stderr is not retained on user-stop.)\n";
		ArchiveLog(missionPath, note, "stopped-");
	}

	EditorTaskRunner::CancelTask(s_task);
	// State transition happens in OnCancelled via PumpMainThread.
}

const char* StatusLine()
{
	return s_status;
}

char* GameExePathBuffer()
{
	ResolveExe();
	return s_exePath;
}

int GameExePathBufferSize()
{
	return (int)sizeof(s_exePath);
}

} // namespace EditorPlaytest
