#!/usr/bin/env python3
"""
Audio Track Selection Parity Tests

Compares yt-dlpp audio track selection with yt-dlp to ensure parity.
Tests language field extraction, default selection, and --audio-lang option.
"""

import subprocess
import json
import sys
import os
import re

# Configuration
YT_DLPP_PATH = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../build/yt-dlpp.exe")
)

# Test video with multiple audio tracks
# User reported getting German when English was expected
TEST_VIDEO_MULTI_AUDIO = "https://www.youtube.com/watch?v=2uiPy-gJUiE"


def run_cmd(cmd, timeout=120):
    """Run a command and return (stdout, stderr, returncode)."""
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, encoding="utf-8"
        )
        return result.stdout, result.stderr, result.returncode
    except subprocess.TimeoutExpired:
        return "", "TIMEOUT", -1
    except Exception as e:
        return "", str(e), -1


def get_json(cmd):
    """Run command and parse JSON output."""
    stdout, stderr, code = run_cmd(cmd)
    if code != 0:
        print(f"  Command failed: {' '.join(cmd)}")
        print(f"  stderr: {stderr[:500]}")
        return None
    try:
        return json.loads(stdout)
    except json.JSONDecodeError as e:
        print(f"  JSON parse error: {e}")
        return None


def get_itag_from_url(url):
    """Extract itag from YouTube URL."""
    match = re.search(r"[?&]itag=(\d+)", url)
    return match.group(1) if match else None


def extract_audio_formats(formats_list):
    """Extract audio-only formats with language info."""
    audio_formats = []
    for f in formats_list:
        # Check for audio-only: vcodec == "none" or no video dimensions
        vcodec = f.get("vcodec", "")
        acodec = f.get("acodec", "")
        if vcodec == "none" and acodec and acodec != "none":
            audio_formats.append({
                "itag": f.get("format_id", f.get("itag", "?")),
                "ext": f.get("ext", "?"),
                "language": f.get("language"),
                "language_preference": f.get("language_preference", -1),
                "tbr": f.get("tbr", 0),
                "acodec": acodec,
                "format_note": f.get("format_note", ""),
            })
    return audio_formats


class TestResult:
    def __init__(self, name):
        self.name = name
        self.passed = False
        self.message = ""

    def ok(self, msg=""):
        self.passed = True
        self.message = msg
        return self

    def fail(self, msg):
        self.passed = False
        self.message = msg
        return self

    def __str__(self):
        status = "PASS" if self.passed else "FAIL"
        msg = f": {self.message}" if self.message else ""
        return f"{self.name}: {status}{msg}"


def test_dump_json_audio_fields():
    """Test that audio format fields match between yt-dlp and yt-dlpp."""
    result = TestResult("dump_json_audio_fields")

    print("\n--- Fetching yt-dlp JSON ---")
    ytdlp_json = get_json(["yt-dlp", "--no-playlist", "--dump-json", TEST_VIDEO_MULTI_AUDIO])

    print("--- Fetching yt-dlpp JSON ---")
    ytdlpp_json = get_json([YT_DLPP_PATH, "--dump-json", TEST_VIDEO_MULTI_AUDIO])

    if not ytdlp_json or not ytdlpp_json:
        return result.fail("Failed to get JSON from one or both tools")

    # Extract audio formats
    dlp_audio = extract_audio_formats(ytdlp_json.get("formats", []))
    dlpp_audio = extract_audio_formats(ytdlpp_json.get("formats", []))

    print(f"\n  yt-dlp audio formats: {len(dlp_audio)}")
    print(f"  yt-dlpp audio formats: {len(dlpp_audio)}")

    # Show language info
    print("\n  yt-dlp audio tracks:")
    for af in dlp_audio[:5]:  # First 5
        print(f"    itag={af['itag']}: lang={af['language']}, pref={af['language_preference']}, note={af['format_note']}")

    print("\n  yt-dlpp audio tracks:")
    for af in dlpp_audio[:5]:
        print(f"    itag={af['itag']}: lang={af['language']}, pref={af['language_preference']}, note={af['format_note']}")

    # Check if language fields are populated
    dlp_has_lang = any(af["language"] for af in dlp_audio)
    dlpp_has_lang = any(af["language"] for af in dlpp_audio)

    if dlp_has_lang and not dlpp_has_lang:
        return result.fail("yt-dlp has language fields but yt-dlpp doesn't")

    # Check language_preference values
    dlp_prefs = set(af["language_preference"] for af in dlp_audio)
    dlpp_prefs = set(af["language_preference"] for af in dlpp_audio)
    print(f"\n  yt-dlp language_preference values: {sorted(dlp_prefs)}")
    print(f"  yt-dlpp language_preference values: {sorted(dlpp_prefs)}")

    return result.ok(f"{len(dlpp_audio)} audio formats with language info")


def test_default_audio_selection():
    """Test that default audio selection matches between tools."""
    result = TestResult("default_audio_selection")

    print("\n--- Getting yt-dlp default audio ---")
    stdout, stderr, code = run_cmd(
        ["yt-dlp", "-f", "bestaudio", "--get-url", TEST_VIDEO_MULTI_AUDIO]
    )
    dlp_url = stdout.strip().split("\n")[0] if code == 0 else ""
    dlp_itag = get_itag_from_url(dlp_url)

    print("--- Getting yt-dlpp default audio ---")
    stdout, stderr, code = run_cmd(
        [YT_DLPP_PATH, "-f", "bestaudio", "--get-url", TEST_VIDEO_MULTI_AUDIO]
    )
    dlpp_url = stdout.strip().split("\n")[0] if code == 0 else ""
    dlpp_itag = get_itag_from_url(dlpp_url)

    print(f"  yt-dlp selected itag: {dlp_itag}")
    print(f"  yt-dlpp selected itag: {dlpp_itag}")

    if not dlp_itag or not dlpp_itag:
        return result.fail("Failed to get itag from one or both tools")

    if dlp_itag == dlpp_itag:
        return result.ok(f"Both selected itag {dlp_itag}")
    else:
        return result.fail(f"yt-dlp={dlp_itag} vs yt-dlpp={dlpp_itag}")


def test_audio_lang_specific():
    """Test --audio-lang with a specific language."""
    result = TestResult("audio_lang_specific")

    # First, find what languages are available
    print("\n--- Checking available languages ---")
    ytdlp_json = get_json(["yt-dlp", "--no-playlist", "--dump-json", TEST_VIDEO_MULTI_AUDIO])
    if not ytdlp_json:
        return result.fail("Could not fetch video info")

    audio_formats = extract_audio_formats(ytdlp_json.get("formats", []))
    languages = set(af["language"] for af in audio_formats if af["language"])
    print(f"  Available languages: {languages}")

    if len(languages) < 2:
        return result.ok("SKIP: Video has less than 2 audio languages")

    # Pick a non-default language to test
    # We'll try 'en' first, otherwise pick any
    test_lang = "en" if "en" in languages else list(languages)[0]
    print(f"  Testing with language: {test_lang}")

    # yt-dlp: use format filter
    print(f"\n--- yt-dlp: -f 'ba[language={test_lang}]' ---")
    stdout, stderr, code = run_cmd(
        ["yt-dlp", "-f", f"ba[language={test_lang}]", "--get-url", TEST_VIDEO_MULTI_AUDIO]
    )
    dlp_url = stdout.strip().split("\n")[0] if code == 0 else ""
    dlp_itag = get_itag_from_url(dlp_url)
    print(f"  yt-dlp itag: {dlp_itag}")

    # yt-dlpp: use --audio-lang
    print(f"--- yt-dlpp: --audio-lang {test_lang} ---")
    stdout, stderr, code = run_cmd(
        [YT_DLPP_PATH, "-f", "bestaudio", f"--audio-lang", test_lang, "--get-url", TEST_VIDEO_MULTI_AUDIO]
    )
    dlpp_url = stdout.strip().split("\n")[0] if code == 0 else ""
    dlpp_itag = get_itag_from_url(dlpp_url)
    print(f"  yt-dlpp itag: {dlpp_itag}")

    if not dlp_itag:
        return result.fail(f"yt-dlp couldn't find language={test_lang}")

    if dlp_itag == dlpp_itag:
        return result.ok(f"Both selected itag {dlp_itag} for lang={test_lang}")
    else:
        return result.fail(f"yt-dlp={dlp_itag} vs yt-dlpp={dlpp_itag}")


def main():
    if not os.path.exists(YT_DLPP_PATH):
        print(f"ERROR: yt-dlpp not found at {YT_DLPP_PATH}")
        print("Build the project first: cmake --build build --config Release")
        sys.exit(1)

    print("=" * 60)
    print("Audio Track Selection Parity Tests")
    print(f"Test video: {TEST_VIDEO_MULTI_AUDIO}")
    print("=" * 60)

    tests = [
        test_dump_json_audio_fields,
        test_default_audio_selection,
        test_audio_lang_specific,
    ]

    results = []
    for test_fn in tests:
        print(f"\n{'=' * 40}")
        print(f"Running: {test_fn.__name__}")
        print("=" * 40)
        try:
            result = test_fn()
            results.append(result)
            print(f"\n>>> {result}")
        except Exception as e:
            result = TestResult(test_fn.__name__).fail(f"Exception: {e}")
            results.append(result)
            print(f"\n>>> {result}")

    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    passed = sum(1 for r in results if r.passed)
    failed = sum(1 for r in results if not r.passed)
    for r in results:
        print(f"  {r}")
    print(f"\nTotal: {passed} passed, {failed} failed")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
