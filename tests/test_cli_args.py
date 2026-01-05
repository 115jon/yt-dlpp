import subprocess
import os
import sys

# Constants
YT_DLPP_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/yt-dlpp.exe"))
TEST_URL = "https://www.youtube.com/watch?v=9H8XGQqXRqA"

def run_yt_dlpp(args):
    """Runs yt-dlpp with the given arguments."""
    cmd = [YT_DLPP_PATH] + args
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result

def test_positional_url():
    """Test that yt-dlpp accepts the URL as a positional argument."""
    # Run with positional URL and --get-url to verify it parses correctly
    # effectively matching: yt-dlpp --get-url "URL"
    result = run_yt_dlpp(["--get-url", TEST_URL])

    if result.returncode != 0:
        print("FAIL: Process returned non-zero exit code.")
        print("Stderr:", result.stderr)
        return False

    # Check if the output looks like a URL (should start with http)
    output_lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]

    # Filter out potential logs if mixing stdout/stderr (though we captured mostly stdout)
    # The fix ensures it doesn't print "Please provide a URL..."
    if "Please provide a URL using --url <link>" in result.stdout:
        print("FAIL: Logic still requests --url flag.")
        return False

    # We expect at least one valid URL in the output
    valid_url_found = any(line.startswith("http") for line in output_lines)

    if valid_url_found:
        print("PASS: Positional URL accepted and processed.")
        return True
    else:
        print("FAIL: No URL found in output (possibly extraction failed, but argument parsing should have worked).")
        print("Stdout:", result.stdout)
        return False

if __name__ == "__main__":
    if not os.path.exists(YT_DLPP_PATH):
        print(f"Error: yt-dlpp executable not found at {YT_DLPP_PATH}")
        sys.exit(1)

    if test_positional_url():
        sys.exit(0)
    else:
        sys.exit(1)
