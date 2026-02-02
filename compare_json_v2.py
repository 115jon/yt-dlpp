import json
import re

def find_json(text):
    # Find the largest substring that is valid JSON
    # Start from '{' and end at the last '}'
    start = text.find('{')
    if start == -1: return None

    # Try parsing progressively shorter substrings from the end
    for end in range(len(text), start, -1):
        try:
            return json.loads(text[start:end])
        except json.JSONDecodeError:
            continue
    return None

def analyze(f1_name, f2_name):
    with open(f1_name, 'r', encoding='utf-8') as f:
        j1 = find_json(f.read())
    with open(f2_name, 'r', encoding='utf-8') as f:
        j2 = find_json(f.read())

    if not j1 or not j2:
        print(f"Failed to load JSON from {f1_name} or {f2_name}")
        return

    f1 = j1.get('formats', [])
    f2 = j2.get('formats', [])

    print(f"yt-dlp formats: {len(f1)}")
    print(f"yt-dlpp formats: {len(f2)}")

    def get_fid(f):
        return f.get('format_id') or str(f.get('itag', ''))

    fids1 = sorted(list(set([get_fid(f) for f in f1])))
    fids2 = sorted(list(set([get_fid(f) for f in f2])))

    print(f"yt-dlp format_ids: {fids1}")
    print(f"yt-dlpp format_ids: {fids2}")

    missing_in_dlpp = [it for it in fids1 if it not in fids2]
    extra_in_dlpp = [it for it in fids2 if it not in fids1]

    print(f"Missing in yt-dlpp: {missing_in_dlpp}")
    print(f"Extra in yt-dlpp: {extra_in_dlpp}")

    # Best format comparison
    best1 = j1.get('format_id')
    best2 = j2.get('format_id')
    print(f"\nyt-dlp selected format: {best1}")
    print(f"yt-dlpp selected format: {best2}")

if __name__ == "__main__":
    import sys
    f1 = sys.argv[1] if len(sys.argv) > 1 else 'yt_dlp_vid.json'
    f2 = sys.argv[2] if len(sys.argv) > 2 else 'yt_dlpp_vid.json'
    analyze(f1, f2)
