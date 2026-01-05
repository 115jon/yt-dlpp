
import subprocess
import requests

def test_url_extraction(video_url, format_code="bestaudio"):
    print(f"Testing URL: {video_url} with format: {format_code}")

    # 1. Run yt-dlp
    print("Running yt-dlp...")
    try:
        ytdlp_cmd = [
            "yt-dlp",
            "--no-playlist",
            "--quiet",
            "--no-warnings",
            "--get-url",
            "-f", format_code,
            video_url
        ]
        ytdlp_res = subprocess.check_output(ytdlp_cmd, text=True).strip()
        print(f"yt-dlp URL found (Length: {len(ytdlp_res)})")
    except subprocess.CalledProcessError as e:
        print(f"yt-dlp failed: {e}")
        ytdlp_res = None
    except FileNotFoundError:
        print("yt-dlp not found in PATH")
        ytdlp_res = None

    # 2. Run yt-dlpp
    print("Running yt-dlpp...")
    try:
        ytdlpp_cmd = [
            r"f:/dev/C++/yt-dlpp/build/yt-dlpp.exe",
            "--url", video_url,
            "--get-url",
            "-f", format_code
        ]
        ytdlpp_res = subprocess.check_output(ytdlpp_cmd, text=True).strip()
        print(f"yt-dlpp URL found (Length: {len(ytdlpp_res)})")
    except subprocess.CalledProcessError as e:
        print(f"yt-dlpp failed: {e}")
        ytdlpp_res = None

    # 3. Validation
    if ytdlp_res:
        check_url(ytdlp_res, "yt-dlp")

    if ytdlpp_res:
        check_url(ytdlpp_res, "yt-dlpp")

    if ytdlp_res and ytdlpp_res:
        # Compare components if possible?
        pass

def check_url(url, label):
    try:
        # Use HEAD request. Note: YouTube URLs might require ranges or specific headers often,
        # but usually HEAD works for validity check.
        # Sometimes 403 happens if headers not perfect, but let's see.
        headers = {
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"
        }
        r = requests.head(url, headers=headers, allow_redirects=True)
        print(f"[{label}] Status: {r.status_code}")
        if r.status_code in [200, 206]:
            print(f"[{label}] SUCCESS: URL is valid and accessible.")
        elif r.status_code == 403:
            print(f"[{label}] WARNING: 403 Forbidden. (This often happens if IP is bound or n-sig failed)")
        else:
            print(f"[{label}] FAILED: {r.status_code}")
    except Exception as e:
        print(f"[{label}] EXCEPTION: {e}")

if __name__ == "__main__":
    # Rick Astley
    # test_url_extraction("https://www.youtube.com/watch?v=dQw4w9WgXcQ")

    # Specific video requested by user
    test_url_extraction("https://www.youtube.com/watch?v=R_sqERohYBI")
