import json
import sys
import difflib

def load_json(filename):
    try:
        with open(filename, 'r', encoding='utf-8') as f:
            content = f.read()
            # Try to find JSON object start
            idx = content.find('{')
            if idx == -1: return None
            return json.loads(content[idx:])
    except Exception as e:
        print(f"Error reading {filename}: {e}")
        return None

def normalize(obj):
    """Normalize JSON for comparison by removing dynamic keys."""
    if isinstance(obj, dict):
        return {
            k: normalize(v) for k, v in obj.items()
            if k not in [
                'http_headers', 'epoch', 'upload_date', 'thumbnails',
                'down_speed', 'eta', 'elapsed', 'total_bytes_estimate',
                '_elapsed_str', '_total_bytes_estimate_str', 'filename',
                'timestamp' # Add other dynamic fields here
            ]
        }
    elif isinstance(obj, list):
        return [normalize(x) for x in obj]
    else:
        return obj

def compare(file1, file2):
    j1 = load_json(file1)
    j2 = load_json(file2)

    if not j1 or not j2:
        print("Failed to parse JSONs")
        return 1

    # Normalize to remove noise
    # j1_norm = normalize(j1)
    # j2_norm = normalize(j2)

    # Structure check: Compare Keys
    keys1 = set(j1.keys())
    keys2 = set(j2.keys())

    missing = keys1 - keys2
    extra = keys2 - keys1

    if missing:
        print(f"MISSING KEYS in {file2}: {sorted(list(missing))}")
    if extra:
        print(f"EXTRA KEYS in {file2}: {sorted(list(extra))}")

    # Formats check
    f1 = j1.get('formats', [])
    f2 = j2.get('formats', [])
    print(f"Format Counts: yt-dlp={len(f1)}, yt-dlpp={len(f2)}")

    # ID check
    def get_ids(formats):
        return sorted([f.get('format_id') or str(f.get('itag')) for f in formats])

    ids1 = get_ids(f1)
    ids2 = get_ids(f2)

    if ids1 != ids2:
        print("Format ID Mismatch:")
        import difflib
        for line in difflib.unified_diff(ids1, ids2, fromfile='yt-dlp', tofile='yt-dlpp'):
            print(line)
    else:
        print("Format IDs match exactly.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        # Default behavior for dev loop
        compare('yt_dlp_out.json', 'yt_dlpp_out.json')
    else:
        compare(sys.argv[1], sys.argv[2])
