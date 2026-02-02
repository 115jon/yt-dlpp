import subprocess
import os
import time
import glob
import sys

# Windows path handling
YTDLPP = os.path.abspath("build/yt-dlpp.exe")
URL = "https://www.youtube.com/watch?v=jNQXAC9IVRw"
OUTPUT_TEMPLATE = "part_test_%(id)s.%(ext)s"

def test_part_file_mechanics():
    # Cleanup
    for f in glob.glob("part_test_*"):
        try:
            os.remove(f)
        except OSError:
            pass

    print(f"Starting download of {URL}...")
    print(f"Using executable: {YTDLPP}")

    proc = subprocess.Popen(
        [YTDLPP, URL, "-o", OUTPUT_TEMPLATE],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    saw_part_file = False

    start_time = time.time()
    while proc.poll() is None:
        # Check for .part file
        part_files = glob.glob("part_test_*.part")
        if part_files:
            if not saw_part_file:
                print(f"Seen part file: {part_files[0]}")
                saw_part_file = True
        time.sleep(0.05)
        if time.time() - start_time > 60:
            proc.kill()
            raise TimeoutError("Download took too long")

    stdout, stderr = proc.communicate()

    if proc.returncode != 0:
        print("STDOUT:", stdout)
        print("STDERR:", stderr)
        raise RuntimeError("yt-dlpp failed")

    # Verification
    final_files = glob.glob("part_test_*")
    # Filter out any stray parts if they exist (for final count check)
    real_files = [f for f in final_files if not f.endswith('.part')]

    if len(real_files) != 1:
        print("Files found:", final_files)
        print("STDOUT:", stdout)
        print("STDERR:", stderr)
        raise AssertionError(f"Expected exactly one final file, found {len(real_files)}")

    final_file = real_files[0]
    print(f"Final file: {final_file}")

    # Check for .part leftovers
    part_leftovers = glob.glob("part_test_*.part")
    if part_leftovers:
         raise AssertionError(f".part file was not cleaned up: {part_leftovers}")

    if not saw_part_file:
        print("FileSystem check missed the .part file (too fast?). Checking logs...")
        # Start looking for ".part" in the logs
        if ".part" in stdout or ".part" in stderr:
            print("Logs confirm .part usage.")
        else:
            print("STDOUT:", stdout)
            print("STDERR:", stderr)
            raise AssertionError("Did not see .part usage in logs or filesystem")
    else:
        print("FileSystem verification confirming .part existence passed.")

    print("Success!")

if __name__ == "__main__":
    test_part_file_mechanics()
