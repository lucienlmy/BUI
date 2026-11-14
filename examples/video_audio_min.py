"""The smallest video with sound: a window with a video and its controls.

    python examples/video_audio_min.py [clip.mp4]

The controls under the picture have play / pause, a scrubber and, once the
sound is playing, a speaker button that mutes it.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bindings", "python"))

from bui.components import App, Video  # noqa: E402

if len(sys.argv) > 1:
    clip = " ".join(sys.argv[1:])  # a path with spaces may arrive split into words
    if not os.path.isfile(clip):
        sys.exit(f"no such video file: {clip}\n(quote a path with spaces)")
else:
    clip = os.path.join(HERE, "..", "tests", "videos", "clip_audio.mp4")
App("Video with sound", (400, 260), Video(clip, controls=True, loop=True)).run()
