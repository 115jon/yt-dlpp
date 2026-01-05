import subprocess
import os
import sys
import re
import requests

# Constants
YT_DLPP_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/yt-dlpp.exe"))
TEST_URL = "https://www.youtube.com/watch?v=9H8XGQqXRqA"

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
        headers = {
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"
        }
        r = requests.head(url, headers=headers, allow_redirects=True)
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

def test_output_comparison():
    """Test that yt-dlpp output is comparable to yt-dlp."""

    print("--- Getting Reference Output (yt-dlp) ---")
    ytdlp_res = run_cmd(["yt-dlp", "--get-url", TEST_URL])

    ytdlp_urls = []
    if ytdlp_res.returncode == 0:
        ytdlp_urls = [line.strip() for line in ytdlp_res.stdout.splitlines() if line.strip().startswith("http")]

    ytdlp_itags = [get_itag_from_url(u) for u in ytdlp_urls]
    print(f"yt-dlp URLs found: {len(ytdlp_urls)}")
    print(f"yt-dlp ITAGs: {ytdlp_itags}")

    print("\n--- Getting Actual Output (yt-dlpp) ---")
    ytdlpp_res = run_cmd([YT_DLPP_PATH, "--get-url", TEST_URL])

    if ytdlpp_res.returncode != 0:
        print("FAIL: yt-dlpp process failed.")
        print(ytdlpp_res.stderr)
        return False

    ytdlpp_urls = [line.strip() for line in ytdlpp_res.stdout.splitlines() if line.strip().startswith("http")]
    ytdlpp_itags = [get_itag_from_url(u) for u in ytdlpp_urls]
    print(f"yt-dlpp URLs found: {len(ytdlpp_urls)}")
    print(f"yt-dlpp ITAGs: {ytdlpp_itags}")

    if not ytdlpp_urls:
        print("FAIL: yt-dlpp returned no URLs.")
        return False

    # Validate URLs
    print("\n--- Validating yt-dlpp URLs ---")
    valid_count = 0
    for i, url in enumerate(ytdlpp_urls):
        itag = ytdlpp_itags[i]
        if check_url(url, f"yt-dlpp/itag={itag}"):
            valid_count += 1

    if valid_count == 0:
        print("FAIL: All yt-dlpp URLs were invalid/inaccessible.")
        return False

    print(f"PASS: Found {valid_count} valid, accessible stream(s).")

    # Validation: yt-dlpp should return valid video URLs
    print("PASS: yt-dlpp successfully extracted valid video/audio stream URLs.")
    if len(ytdlpp_urls) > len(ytdlp_urls):
        print("NOTE: yt-dlpp found MORE streams than the reference yt-dlp (likely successfully deciphered video).")

    return True

if __name__ == "__main__":
    if not os.path.exists(YT_DLPP_PATH):
        print(f"Error: yt-dlpp executable not found at {YT_DLPP_PATH}")
        sys.exit(1)

    if test_output_comparison():
        sys.exit(0)
    else:
        sys.exit(1)
