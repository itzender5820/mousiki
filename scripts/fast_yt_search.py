#!/usr/bin/env python3
import urllib.request
import json
import sys
import re

def search(query, limit=5):
    url = "https://www.youtube.com/youtubei/v1/search"
    headers = {
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
    }
    data = {
        "context": {
            "client": {
                "clientName": "WEB",
                "clientVersion": "2.20210721.00.00"
            }
        },
        "query": query
    }
    
    req = urllib.request.Request(url, data=json.dumps(data).encode("utf-8"), headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            res = json.loads(response.read().decode())
    except Exception as e:
        print("{}", file=sys.stderr)
        return

    contents = res.get("contents", {}).get("twoColumnSearchResultsRenderer", {}).get("primaryContents", {}).get("sectionListRenderer", {}).get("contents", [])
    if not contents:
        return
        
    items = contents[0].get("itemSectionRenderer", {}).get("contents", [])
    
    count = 0
    for item in items:
        video = item.get("videoRenderer")
        if not video:
            continue
        vid_id = video.get("videoId")
        title = video.get("title", {}).get("runs", [{}])[0].get("text", "")
        uploader = video.get("ownerText", {}).get("runs", [{}])[0].get("text", "")
        
        if vid_id and title:
            # Output in yt-dlp flat-playlist json format for compatibility
            out = {
                "id": vid_id,
                "title": title,
                "uploader": uploader
            }
            print(json.dumps(out))
            count += 1
            if count >= limit:
                break

if __name__ == "__main__":
    query = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    search(query, limit)
