import time
import subprocess
import os
import shutil

# Config
URL = "https://www.youtube.com/watch?v=R_sqERohYBI"
OUTPUT_DIR = "tests/benchmark_output"
YT_DLPP_EXE = "build/yt-dlpp.exe"

def run_command(cmd, cwd=None):
    start_time = time.time()
    try:
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=True,
            cwd=cwd
        )
        stdout, stderr = process.communicate()
        end_time = time.time()
        return end_time - start_time, stdout.decode('utf-8', errors='ignore'), stderr.decode('utf-8', errors='ignore'), process.returncode
    except Exception as e:
        return 0, "", str(e), -1

def main():
    if os.path.exists(OUTPUT_DIR):
        shutil.rmtree(OUTPUT_DIR)
    os.makedirs(OUTPUT_DIR)

    print(f"Benchmarking URL: {URL}")

    # Test 1: yt-dlp (Python)
    print("\n--- Running yt-dlp (Reference) ---")
    dlp_cmd = f"yt-dlp -f bestaudio --output \"{OUTPUT_DIR}/%(id)s_ref.%(ext)s\" {URL}"
    dlp_time, dlp_out, dlp_err, dlp_code = run_command(dlp_cmd)

    if dlp_code == 0:
        print(f"yt-dlp finished in {dlp_time:.2f} seconds")
    else:
        print(f"yt-dlp failed in {dlp_time:.2f} seconds")
        print("Error:", dlp_err)

    # Test 2: yt-dlpp (C++)
    print("\n--- Running yt-dlpp (Target) ---")
    # Note: yt-dlpp doesn't support complex output templates yet, so we cd into dir or just let it download to CWD and move it?
    # It downloads to CWD. Let's run it inside the output dir.

    # We need absolute path to exe
    exe_path = os.path.abspath(YT_DLPP_EXE)

    dlpp_cmd = f"\"{exe_path}\" --format bestaudio --url {URL}"
    dlpp_time, dlpp_out, dlpp_err, dlpp_code = run_command(dlpp_cmd, cwd=OUTPUT_DIR)

    if dlpp_code == 0:
        print(f"yt-dlpp finished in {dlpp_time:.2f} seconds")
    else:
        print(f"yt-dlpp failed in {dlpp_time:.2f} seconds")
        # print(dlpp_out) # Log might be long
        print("Last 10 lines of Output:")
        lines = dlpp_out.splitlines()[-10:]
        for l in lines:
            print(l)

    print("\n--- Results ---")
    if dlp_code == 0 and dlpp_code == 0:
        print(f"Reference: {dlp_time:.2f}s")
        print(f"Target:    {dlpp_time:.2f}s")
        if dlpp_time > dlp_time:
            print(f"Target is {dlpp_time/dlp_time:.2f}x slower")
        else:
            print(f"Target is {dlp_time/dlpp_time:.2f}x faster")
    else:
        print("Could not compare due to failures.")

if __name__ == "__main__":
    main()
