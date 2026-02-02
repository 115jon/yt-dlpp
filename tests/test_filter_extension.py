import subprocess
import json
import sys
import os

# Configuration
YTDLP_PATH = "yt-dlp"
YTDLPP_PATH = "./build/yt-dlpp.exe"
TEST_URL = "https://www.youtube.com/watch?v=jNQXAC9IVRw"

def run_cmd(cmd):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True, encoding='utf-8')
        return result.stdout
    except subprocess.CalledProcessError as e:
        print(f"Error running command: {cmd}")
        print(f"STDERR: {e.stderr}")
        return None

def main():
    print(f"Testing format selector: best[ext=mp4]")

    # Use best[ext=mp4] which forces mp4 container.
    # Note: yt-dlp might warn about pre-merged formats, but should return one.
    selector = "best[ext=mp4]"

    # Get Reference
    print("Running yt-dlp...")
    ref_json = run_cmd([YTDLP_PATH, "-j", "-f", selector, TEST_URL])
    if not ref_json:
        print("Failed to run yt-dlp")
        sys.exit(1)

    ref_data = json.loads(ref_json)
    ref_ext = ref_data.get('ext')
    print(f"Reference ext: {ref_ext}")

    # Get Target
    print("Running yt-dlpp...")
    target_json = run_cmd([YTDLPP_PATH, "-j", "-f", selector, TEST_URL])
    if not target_json:
        print("Failed to run yt-dlpp")
        # Try capture stderr
        res = subprocess.run([YTDLPP_PATH, "-j", "-f", selector, TEST_URL], capture_output=True, text=True)
        print(f"Target STDERR: {res.stderr}")
        sys.exit(1)

    target_data = json.loads(target_json)
    target_ext = target_data.get('ext')
    print(f"Target ext: {target_ext}")

    if ref_ext != target_ext:
        print(f"MISMATCH! Ref: {ref_ext} vs Target: {target_ext}")
        sys.exit(1)

    if target_ext != "mp4":
         print(f"WARNING: Expected mp4, got {target_ext}")
         sys.exit(1)

    print("SUCCESS: Extensions match.")
    sys.exit(0)
