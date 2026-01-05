import subprocess
import json
import sys

def run_cmd(cmd):
    try:
        result = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL, encoding='utf-8')
        return json.loads(result)
    except subprocess.CalledProcessError as e:
        print(f"Error running {cmd[0]}: {e}")
        return None
    except json.JSONDecodeError as e:
        print(f"Error decoding JSON from {cmd[0]}: {e}")
        return None

def compare_json(url):
    print(f"Comparing JSON output for: {url}")

    # Run yt-dlp
    print("Fetching yt-dlp JSON...")
    ytdlp_json = run_cmd(["yt-dlp", "--no-playlist", "--dump-json", url])

    # Run yt-dlpp
    print("Fetching yt-dlpp JSON...")
    ytdlpp_cmd = [r"f:/dev/C++/yt-dlpp/build/yt-dlpp.exe", "--url", url, "--dump-json"]
    ytdlpp_json = run_cmd(ytdlpp_cmd)

    if not ytdlp_json or not ytdlpp_json:
        print("Failed to get JSON from one or both tools.")
        return

    # --- Root Key Comparison ---
    print("\n--- Root Keys Analysis ---")
    keys_dlp = set(ytdlp_json.keys())
    keys_dlpp = set(ytdlpp_json.keys())

    missing_keys = keys_dlp - keys_dlpp
    extra_keys = keys_dlpp - keys_dlp

    print(f"yt-dlp keys: {len(keys_dlp)}")
    print(f"yt-dlpp keys: {len(keys_dlpp)}")
    print(f"Missing in yt-dlpp ({len(missing_keys)}): {sorted(list(missing_keys))[:10]}...") # Show first 10

    # --- Format Comparison ---
    print("\n--- Format Comparison ---")
    formats_dlp = {f.get('format_id'): f for f in ytdlp_json.get('formats', [])}
    formats_dlpp = {f.get('format_id'): f for f in ytdlpp_json.get('formats', [])}

    print(f"Total Formats Found via yt-dlp: {len(formats_dlp)}")
    print(f"Total Formats Found via yt-dlpp: {len(formats_dlpp)}")

    common_ids = set(formats_dlp.keys()) & set(formats_dlpp.keys())
    print(f"Common Formats: {len(common_ids)}")

    if common_ids:
        fid = list(common_ids)[0]
        print(f"\nAnalyzing Common Format ID: {fid}")
        f_dlp = formats_dlp[fid]
        f_dlpp = formats_dlpp[fid]

        f_keys_dlp = set(f_dlp.keys())
        f_keys_dlpp = set(f_dlpp.keys())

        f_missing = f_keys_dlp - f_keys_dlpp
        print(f"Format Keys Missing in yt-dlpp ({len(f_missing)}): {sorted(list(f_missing))[:10]}...")

        # Check specific values
        sub_fields = ["ext", "width", "height", "filesize", "vcodec", "acodec", "tbr", "abr", "vbr", "fps"]
        for sf in sub_fields:
            if sf in f_dlp and sf in f_dlpp:
                v_dlp = f_dlp.get(sf)
                v_dlpp = f_dlpp.get(sf)
                if v_dlp is None and v_dlpp == 0: v_dlp = 0
                match = v_dlp == v_dlpp
                print(f"  {sf}: {'OK' if match else 'FAIL'} ({v_dlp} vs {v_dlpp})")
            elif sf in f_dlp:
                 print(f"  {sf}: MISSING in yt-dlpp (yt-dlp: {f_dlp[sf]})")


if __name__ == "__main__":
    url = "https://www.youtube.com/watch?v=R_sqERohYBI"
    if len(sys.argv) > 1:
        url = sys.argv[1]
    compare_json(url)
