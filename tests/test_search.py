#!/usr/bin/env python3
"""
Test script for YouTube search functionality.
This tests the search feature that was reported to fail with 403 errors.
Validates that extracted URLs are actually accessible (not 403).
"""

import os
import re
import subprocess
import sys

import requests

# Constants
YT_DLPP_PATH = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "../build/yt-dlpp.exe"))


def run_cmd(cmd):
    """Runs a command and returns the stdout and return code."""
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result


def get_itag_from_url(url):
    """Extracts the itag parameter from a YouTube stream URL."""
    match = re.search(r"[?&]itag=(\d+)", url)
    if match:
        return match.group(1)
    return None


def check_url(url, label):
    """Checks if a URL is accessible via HTTP HEAD request."""
    try:
        # Use the same headers as yt-dlp for validation
        # This matches the Android client headers used by yt-dlp
        headers = {
            "User-Agent": "com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip",
            "Accept": "*/*",
            "Accept-Encoding": "gzip, deflate",
            "Referer": "https://www.youtube.com/"
        }
        r = requests.head(url, headers=headers,
                          allow_redirects=True, timeout=10)
        print(f"[{label}] Status: {r.status_code}")
        if r.status_code in [200, 206]:
            print(f"[{label}] SUCCESS: URL is valid and accessible.")
            return True
        elif r.status_code == 403:
            print(f"[{label}] WARNING: 403 Forbidden.")
            return False
        else:
            print(f"[{label}] FAILED: {r.status_code}")
            return False
    except Exception as e:
        print(f"[{label}] EXCEPTION: {e}")
        return False


def test_search(query="ytsearch:hello"):
    """Test that yt-dlpp can perform a YouTube search and return valid URLs."""
    print(f"\n=== Testing YouTube Search: '{query}' ===\n")

    if not os.path.exists(YT_DLPP_PATH):
        print(f"Error: yt-dlpp executable not found at {YT_DLPP_PATH}")
        return False

    # Run yt-dlpp with search query
    cmd = [YT_DLPP_PATH, "--url", query, "--get-url"]
    result = run_cmd(cmd)

    if result.returncode != 0:
        print(f"FAIL: Search failed with return code {result.returncode}")
        print(f"STDERR: {result.stderr}")
        return False

    # Check if we got any output
    urls = [line.strip() for line in result.stdout.splitlines()
            if line.strip().startswith("http")]
    if not urls:
        print("FAIL: No results returned from search")
        return False

    print(f"Search returned {len(urls)} results")

    # Validate URLs are accessible (not 403)
    print("\n--- Validating Search Result URLs ---")
    valid_count = 0
    for i, url in enumerate(urls):
        itag = get_itag_from_url(url)
        if check_url(url, f"itag={itag}"):
            valid_count += 1

    if valid_count == 0:
        print(
            f"FAIL: All {len(urls)} URLs were invalid/inaccessible (403 Forbidden)")
        return False

    print(f"PASS: Found {valid_count}/{len(urls)} valid, accessible stream(s)")
    return True


def test_search_comparison():
    """Compare yt-dlp and yt-dlpp search results for URL accessibility."""
    query = "ytsearch:hello"
    print(f"\n=== Comparing Search Results: '{query}' ===\n")

    # Test yt-dlp
    print("Testing yt-dlp...")
    ytdlp_result = run_cmd(["yt-dlp", "--get-url", query])
    ytdlp_urls = []
    if ytdlp_result.returncode == 0:
        ytdlp_urls = [line.strip() for line in ytdlp_result.stdout.splitlines(
        ) if line.strip().startswith("http")]
    print(f"yt-dlp returned {len(ytdlp_urls)} URLs")

    # Validate yt-dlp URLs
    ytdlp_valid = 0
    for url in ytdlp_urls:
        if check_url(url, "yt-dlp"):
            ytdlp_valid += 1

    # Test yt-dlpp
    print("\nTesting yt-dlpp...")
    ytdlpp_result = run_cmd([YT_DLPP_PATH, "--url", query, "--get-url"])
    ytdlpp_urls = []
    if ytdlpp_result.returncode == 0:
        ytdlpp_urls = [line.strip() for line in ytdlpp_result.stdout.splitlines(
        ) if line.strip().startswith("http")]
    print(f"yt-dlpp returned {len(ytdlpp_urls)} URLs")

    # Validate yt-dlpp URLs
    ytdlpp_valid = 0
    for url in ytdlpp_urls:
        if check_url(url, "yt-dlpp"):
            ytdlpp_valid += 1

    print(f"\n--- Summary ---")
    print(f"yt-dlp: {ytdlp_valid}/{len(ytdlp_urls)} valid URLs")
    print(f"yt-dlpp: {ytdlpp_valid}/{len(ytdlpp_urls)} valid URLs")

    if ytdlpp_valid > 0:
        print("\nPASS: yt-dlpp returned accessible URLs")
        return True
    else:
        print("\nFAIL: yt-dlpp returned no accessible URLs")
        return False


if __name__ == "__main__":
    print("=" * 60)
    print("YouTube Search Test Suite")
    print("=" * 60)

    # Run tests
    test1 = test_search("ytsearch:hello")
    test2 = test_search("ytsearch3:test")
    test3 = test_search_comparison()

    print("\n" + "=" * 60)
    print("Test Summary")
    print("=" * 60)
    print(f"Basic search test: {'PASS' if test1 else 'FAIL'}")
    print(f"Multi-result search test: {'PASS' if test2 else 'FAIL'}")
    print(f"Comparison test: {'PASS' if test3 else 'FAIL'}")

    sys.exit(0 if (test1 and test2 and test3) else 1)
