import subprocess
import json
import sys
import os

# Configuration
YTDLP_PATH = "yt-dlp" # Assumes in PATH or alias
YTDLPP_PATH = "./build/yt-dlpp.exe"
TEST_URL = "https://www.youtube.com/watch?v=jNQXAC9IVRw" # Me at the zoo (short, stable)

def run_cmd(cmd):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True, encoding='utf-8')
        return result.stdout
    except subprocess.CalledProcessError as e:
        print(f"Error running command: {cmd}")
        print(f"STDERR: {e.stderr}")
        return None

def get_format_id(json_output):
    try:
        data = json.loads(json_output)
        return data.get('format_id')
    except json.JSONDecodeError:
        print("Failed to decode JSON")
        return None


def check_keys(data, label):
    # keys = data.keys()
    # print(f"[{label}] Keys: {list(keys)}")
    if 'url' not in data:
        print(f"[{label}] Missing 'url' key!")
    if '+' in data.get('format_id', '') and 'requested_formats' not in data:
        print(f"[{label}] Missing 'requested_formats' for merged format!")

def main():
    print(f"Testing format selector: bestvideo+bestaudio")

    # Get Reference (yt-dlp)
    print("Running yt-dlp...")
    ref_json = run_cmd([YTDLP_PATH, "-j", "-f", "bestvideo+bestaudio", TEST_URL])
    if not ref_json:
        print("Failed to run yt-dlp")
        sys.exit(1)

    ref_data = json.loads(ref_json)
    ref_id = ref_data.get('format_id')
    print(f"Reference format_id: {ref_id}")
    check_keys(ref_data, "Ref")

    # Get Target (yt-dlpp)
    print("Running yt-dlpp...")
    target_json = run_cmd([YTDLPP_PATH, "-j", "-f", "bestvideo+bestaudio", TEST_URL])
    if not target_json:
        print("Failed to run yt-dlpp")
        sys.exit(1)

    target_data = json.loads(target_json)
    target_id = target_data.get('format_id')
    print(f"Target format_id: {target_id}")
    check_keys(target_data, "Target")

    if ref_id != target_id:
        print(f"MISMATCH! Ref: {ref_id} vs Target: {target_id}")
        sys.exit(1)

    # Check for requested_formats structure parity if merged
    if '+' in target_id:
        if 'requested_formats' not in target_data:
             print("FAILURE: Target missing requested_formats.")
             sys.exit(1)

        # Verify requested_formats has 2 elements
        req = target_data['requested_formats']
        if len(req) != 2:
             print(f"FAILURE: requested_formats length {len(req)} != 2")
             sys.exit(1)

        # Check output contains valid urls in requested_formats
        for i, fmt in enumerate(req):
            if not fmt.get('url'):
                print(f"FAILURE: requested_formats[{i}] missing url.")
                sys.exit(1)

        if target_data.get('url'):
             print("WARNING: Target HAS top-level url for merged format (yt-dlp usually removes it).")

    print("SUCCESS: Format IDs match and structure looks correct.")
    sys.exit(0)

if __name__ == "__main__":
    main()
