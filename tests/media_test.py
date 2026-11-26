import json
from fractions import Fraction
from pathlib import Path
import subprocess
import sys
import tempfile


def command(*args):
    return subprocess.check_output(args)


def ffmpeg(*args):
    return command("ffmpeg", "-hide_banner", "-v", "error", "-nostdin", *args)


def probe(path, selector):
    result = command(
        "ffprobe", "-v", "error", "-select_streams", selector, "-show_streams",
        "-show_frames", "-of", "json", str(path),
    )
    return json.loads(result)


def times(path, selector):
    result = probe(path, selector)
    time_base = Fraction(result["streams"][0]["time_base"])
    return [f["best_effort_timestamp"] * time_base for f in result["frames"]]


def rgb(path):
    return ffmpeg("-i", str(path), "-map", "0:v:0", "-pix_fmt", "rgb24",
                  "-fps_mode", "passthrough", "-f", "rawvideo", "-")


def audio(path):
    return ffmpeg("-i", str(path), "-map", "0:a:0", "-f", "s16le", "-")


def verify_media(source, output):
    input_pts, output_pts = times(source, "v:0"), times(output, "v:0")
    assert len(input_pts) == len(output_pts) == 8
    assert len(set(b - a for a, b in zip(input_pts, input_pts[1:]))) > 1
    assert all(abs(a - b) <= Fraction(1, 1000) for a, b in zip(input_pts, output_pts))
    input_audio, output_audio = times(source, "a:0"), times(output, "a:0")
    assert input_audio and abs(input_audio[0] - output_audio[0]) <= Fraction(1, 1000)
    assert audio(source) == audio(output)


def verify_mask(source, output):
    original, restored = rgb(source), rgb(output)
    assert len(original) == len(restored) == 8 * 24 * 32 * 3
    changed = 0
    for i, (before, after) in enumerate(zip(original, restored)):
        pixel = i // 3
        frame, row, column = pixel // (32 * 24), pixel // 32 % 24, pixel % 32
        if 2 <= frame < 6 and 5 <= column < 17 and 4 <= row < 16:
            changed += before != after
        else:
            assert before == after, (frame, row, column)
    assert changed > 0


def main(binary, tmp):
    raw = tmp / "fixture.rgb"
    raw.write_bytes(bytes(
        (3 * frame + 6 * (5 + (x - 5) // 4 * 4 if 5 <= x < 17 and 4 <= y < 16 else x)
         + 2 * (4 + (y - 4) // 3 * 3 if 5 <= x < 17 and 4 <= y < 16 else y) + c * 7) % 256
        for frame in range(8) for y in range(24) for x in range(32) for c in range(3)
    ))

    source = tmp / "source.mkv"
    ffmpeg("-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", "32x24",
           "-framerate", "25", "-i", str(raw), "-itsoffset", "0.08",
           "-f", "lavfi", "-i", "sine=frequency=440:duration=0.4:sample_rate=48000",
           "-vf", "setpts=if(lt(N\\,4)\\,N\\,2*N-3)", "-fps_mode", "vfr",
           "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", str(source))

    lossless = tmp / "restored.mkv"
    command(binary, str(source), str(lossless), "lossless", "export")
    verify_media(source, lossless)
    verify_mask(source, lossless)

    compressed = tmp / "reordered.mp4"
    ffmpeg("-i", str(source), "-c:v", "libx264", "-pix_fmt", "yuv420p", "-bf", "3",
           "-x264-params", "b-adapt=0", "-fps_mode", "passthrough", "-c:a", "aac", str(compressed))
    assert any(f.get("pict_type") == "B" for f in probe(compressed, "v:0")["frames"])
    mp4 = tmp / "restored.mp4"
    command(binary, str(compressed), str(mp4), "mp4", "export")
    verify_media(compressed, mp4)

    cancelled = tmp / "keep.mkv"
    cancelled.write_bytes(b"existing destination")
    command(binary, str(source), str(cancelled), "lossless", "cancel")
    assert cancelled.read_bytes() == b"existing destination"
    assert not list(tmp.glob(".mosaic-export-*"))
    print("Preview, VFR and reordered video, audio copy, exact mask, and export cancellation passed.")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="mosaic-test-") as directory:
        main(str(Path(sys.argv[1]).resolve()), Path(directory))
