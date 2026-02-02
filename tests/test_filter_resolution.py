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
    print(f"Testing format selector: best[height=144]")

    selector = "best[height=144]"

    # Get Reference
    print("Running yt-dlp...")
    ref_json = run_cmd([YTDLP_PATH, "-j", "-f", selector, TEST_URL])
    if not ref_json:
        print("Failed to run yt-dlp")
        sys.exit(1)

    ref_data = json.loads(ref_json)
    ref_height = ref_data.get('height')
    print(f"Reference height: {ref_height}")

    # Get Target
    print("Running yt-dlpp...")
    target_json = run_cmd([YTDLPP_PATH, "-j", "-f", selector, TEST_URL])
    if not target_json:
        print("Failed to run yt-dlpp")
        sys.exit(1)

    target_data = json.loads(target_json)
    target_height = target_data.get('height')
    print(f"Target height: {target_height}")

    if ref_height != target_height:
        print(f"MISMATCH! Ref: {ref_height} vs Target: {target_height}")
        sys.exit(1)

    if target_height != 144:
         print(f"WARNING: Expected 144, got {target_height}")

    print("SUCCESS: Heights match.")
    sys.exit(0)

if __name__ == "__main__":
    main()
