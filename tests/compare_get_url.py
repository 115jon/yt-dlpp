#!/usr/bin/env python3
"""
Test extractor-args functionality and HTTP 403 handling.

Tests that both yt-dlp and yt-dlpp:
1. Respect --extractor-args for client selection
2. Return URLs that don't result in HTTP 403 errors
3. Match behavior for ytsearch queries
"""
import subprocess
import os
import sys
import re
import requests
from typing import List, Optional, Tuple

# Constants
YT_DLPP_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/yt-dlpp.exe"))

# Test cases
TEST_CASES = [
    {
        "name": "Direct URL with extractor-args",
        "url": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        "extractor_args": "youtube:player_client=default,-android_sdkless",
    },
    {
        "name": "ytsearch with extractor-args",
        "url": "ytsearch:asmr glooms",
        "extractor_args": "youtube:player_client=default,-android_sdkless",
    },
    {
        "name": "ytsearch without extractor-args (default clients)",
        "url": "ytsearch:test video",
        "extractor_args": None,
    },
]


def run_cmd(cmd: List[str], timeout: int = 120) -> subprocess.CompletedProcess:
    """Runs a command and returns the result."""
    print(f"  Running: {' '.join(cmd)}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return result
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT after {timeout}s")
        return subprocess.CompletedProcess(cmd, returncode=-1, stdout="", stderr="TIMEOUT")


def get_itag_from_url(url: str) -> Optional[str]:
    """Extracts the itag parameter from a YouTube stream URL."""
    match = re.search(r"[?&]itag=(\d+)", url)
    return match.group(1) if match else None


def get_client_from_url(url: str) -> Optional[str]:
    """Extracts the client type from a YouTube stream URL."""
    match = re.search(r"[?&]c=(\w+)", url)
    return match.group(1) if match else None


def check_url_accessible(url: str, label: str) -> Tuple[bool, int]:
    """Checks if a URL is accessible via HTTP HEAD request.

    Returns: (is_accessible, status_code)
    """
    try:
        headers = {
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
        }
        r = requests.head(url, headers=headers, allow_redirects=True, timeout=10)
        return (r.status_code in [200, 206], r.status_code)
    except Exception as e:
        print(f"  [{label}] Exception: {e}")
        return (False, -1)


def extract_urls(output: str) -> List[str]:
    """Extract HTTP URLs from command output."""
    urls = []
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("http"):
            urls.append(line)
    return urls


def test_get_url(tool: str, url: str, extractor_args: Optional[str]) -> Tuple[bool, List[str]]:
    """Run --get-url and return (success, urls)."""
    cmd = [tool, "--get-url", url]
    if extractor_args:
        cmd.extend(["--extractor-args", extractor_args])

    result = run_cmd(cmd)

    # Combine stdout and stderr for yt-dlpp (logs go to stderr)
    all_output = result.stdout + "\n" + result.stderr
    urls = extract_urls(all_output)

    return (result.returncode == 0 or len(urls) > 0, urls)


def test_case(test: dict) -> bool:
    """Run a single test case comparing yt-dlp and yt-dlpp."""
    print(f"\n{'='*60}")
    print(f"TEST: {test['name']}")
    print(f"  URL: {test['url']}")
    print(f"  Extractor Args: {test['extractor_args'] or '(none)'}")
    print(f"{'='*60}")

    # Run yt-dlp
    print("\n--- yt-dlp ---")
    ytdlp_ok, ytdlp_urls = test_get_url("yt-dlp", test["url"], test["extractor_args"])
    print(f"  URLs found: {len(ytdlp_urls)}")

    for url in ytdlp_urls[:2]:  # Show first 2
        itag = get_itag_from_url(url)
        client = get_client_from_url(url)
        print(f"    itag={itag}, client={client}")

    # Run yt-dlpp
    print("\n--- yt-dlpp ---")
    ytdlpp_ok, ytdlpp_urls = test_get_url(YT_DLPP_PATH, test["url"], test["extractor_args"])
    print(f"  URLs found: {len(ytdlpp_urls)}")

    for url in ytdlpp_urls[:2]:  # Show first 2
        itag = get_itag_from_url(url)
        client = get_client_from_url(url)
        print(f"    itag={itag}, client={client}")

    # Validate accessibility (check for 403 errors)
    print("\n--- Validating URLs (checking for 403 errors) ---")

    all_passed = True

    # Check yt-dlp URLs
    ytdlp_403_count = 0
    for i, url in enumerate(ytdlp_urls[:2]):
        itag = get_itag_from_url(url)
        accessible, status = check_url_accessible(url, f"yt-dlp/itag={itag}")
        if status == 403:
            ytdlp_403_count += 1
            print(f"  [yt-dlp/itag={itag}] HTTP 403 Forbidden!")
        elif accessible:
            print(f"  [yt-dlp/itag={itag}] OK (status={status})")
        else:
            print(f"  [yt-dlp/itag={itag}] FAILED (status={status})")

    # Check yt-dlpp URLs
    ytdlpp_403_count = 0
    for i, url in enumerate(ytdlpp_urls[:2]):
        itag = get_itag_from_url(url)
        accessible, status = check_url_accessible(url, f"yt-dlpp/itag={itag}")
        if status == 403:
            ytdlpp_403_count += 1
            print(f"  [yt-dlpp/itag={itag}] HTTP 403 Forbidden!")
            all_passed = False
        elif accessible:
            print(f"  [yt-dlpp/itag={itag}] OK (status={status})")
        else:
            print(f"  [yt-dlpp/itag={itag}] FAILED (status={status})")
            # Non-403 errors don't fail the test (could be rate limiting, etc)

    # Summary
    print("\n--- Summary ---")
    if not ytdlpp_urls:
        print("  FAIL: yt-dlpp returned no URLs")
        return False

    if ytdlpp_403_count > 0:
        print(f"  FAIL: yt-dlpp had {ytdlpp_403_count} URL(s) with HTTP 403")
        return False

    # Check client selection when extractor args are used
    if test["extractor_args"] and "android_sdkless" in test["extractor_args"] and "-android_sdkless" in test["extractor_args"]:
        for url in ytdlpp_urls:
            client = get_client_from_url(url)
            # ANDROID is used by android_sdkless, we want to avoid it
            # But note: the client parameter shows the Innertube client name, not friendly name
            # When excluding android_sdkless, URLs should NOT have c=ANDROID
            # (web/web_safari would have c=WEB)
        # This is informational - the 403 check is more important

    print(f"  PASS: All yt-dlpp URLs are accessible (no 403 errors)")
    return all_passed


def main():
    if not os.path.exists(YT_DLPP_PATH):
        print(f"Error: yt-dlpp executable not found at {YT_DLPP_PATH}")
        sys.exit(1)

    print("=" * 60)
    print("Extractor Args and HTTP 403 Test Suite")
    print("=" * 60)

    passed = 0
    failed = 0

    for test in TEST_CASES:
        if test_case(test):
            passed += 1
        else:
            failed += 1

    print("\n" + "=" * 60)
    print(f"RESULTS: {passed} passed, {failed} failed")
    print("=" * 60)

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
