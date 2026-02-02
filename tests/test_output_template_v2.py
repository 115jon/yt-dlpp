import subprocess
import json
import sys
import os

def run_command(cmd):
    try:
        # shell=True might help on Windows sometimes
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8', shell=True)
        return result.stdout.strip(), result.returncode
    except Exception as e:
        return str(e), -1

def test_output_template():
    url = "https://www.youtube.com/watch?v=jNQXAC9IVRw"
    print(f"Testing URL: {url}")

    templates = [
        "%(id)s.%(ext)s",
        "%(title)s.%(ext)s",
        "%(uploader)s - %(title)s.%(ext)s",
        "Index: %(playlist_index)s - Duration: %(duration)s.%(ext)s"
    ]

    for tpl in templates:
        print(f"Testing template: '{tpl}'")

        ref_cmd = ["yt-dlp", "--print", "filename", "-o", tpl, "-f", "18", url]
        ref_out, ref_code = run_command(ref_cmd)

        if ref_code != 0:
            print(f"Skipping ref fail: {ref_out}")
            continue

        print(f"Ref: {ref_out}")

        exe_path = os.path.abspath("./build/Release/yt-dlpp.exe")
        if not os.path.exists(exe_path):
             exe_path = os.path.abspath("./build/yt-dlpp.exe")
             if not os.path.exists(exe_path):
                 print(f"Cannot find yt-dlpp executable at {exe_path}")
                 sys.exit(1)

        target_cmd = [exe_path, "--get-filename", "-o", tpl, "-f", "18", url]

        target_out, target_code = run_command(target_cmd)

        if target_code != 0:
            print(f"Target failed output: {target_out}")
            sys.exit(1)

        print(f"Tar: {target_out}")

        if ref_out != target_out:
            print("MISMATCH!")
            sys.exit(1)
        else:
            print("MATCH")

if __name__ == "__main__":
    test_output_template()
