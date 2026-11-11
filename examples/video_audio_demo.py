"""Video with sound (bui.components.Video).

    python examples/video_audio_demo.py                 # the 2 s test clip (a 440 Hz tone, looping)
    python examples/video_audio_demo.py path/to/clip.mp4

What to try:

* the clip plays with its sound on the default output; click the speaker
  button in the video's own controls to mute, or the buttons below
* volume: the slider; the sound follows it live
* speed: the sound pauses at any speed but 1x (the picture goes on at its
  own speed), and picks up again at 1x
* seek: the buttons and the scrubber; the sound jumps with the picture
* Pause / Play hold and resume both
* scroll the video out of view with the "Hide video" toggle: the sound goes
  on, and the picture catches up to it when you show the video again

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bindings", "python"))

import bui  # noqa: E402
from bui.components import (App, Button, Checkbox, Label, NumberSlider, Panel, Row,  # noqa: E402
                            Video)

CLIP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "tests", "videos", "clip_audio.mp4")


def build():
    video = Video(CLIP, controls=True, loop=True, fit="contain")
    player = video.player
    status = Label("")

    def status_text():
        names = {bui.VIDEO_LOADING: "loading", bui.VIDEO_PLAYING: "playing", bui.VIDEO_PAUSED: "paused",
                 bui.VIDEO_ENDED: "ended", bui.VIDEO_ERROR: "error"}
        sound = "sound on" if player.has_audio else "no sound yet (no audio track, or the output is waking)"
        mute = ", muted" if player.muted else ""
        return (f"{names.get(player.state, '?')}, {player.time:.1f} / {player.duration:.1f} s, "
                f"{sound}{mute}")

    def refresh():
        status.set("text", status_text())

    player.on_state = lambda _state: refresh()

    def set_volume(v):
        player.volume = v

    def set_speed(v):
        player.rate = v

    def set_muted(on):
        player.muted = on
        refresh()

    def seek(delta):
        player.time = max(0.0, player.time + delta)

    def hide(on):
        video.set("visible", not on)

    return App(
        "BUI: video with sound", (460, 560),
        video,
        status,
        Panel("Sound",
              NumberSlider("Volume", 1.0, min=0.0, max=1.0, on_change=set_volume),
              Checkbox("Muted", False, on_change=set_muted)),
        Panel("Playback",
              Row(Button("Play", video.play), Button("Pause", video.pause),
                  Button("-1 s", lambda: seek(-1.0)), Button("+1 s", lambda: seek(1.0))),
              NumberSlider("Speed", 1.0, min=0.25, max=4.0, on_change=set_speed),
              Checkbox("Hide video (the sound plays on)", False, on_change=hide)),
    )


if __name__ == "__main__":
    app = build()
    window = app.open()

    def tick():  # the time moves and the sound starts without a state change
        app.root.children[1].set("text", status_text())
        window.after(250, tick)

    window.after(250, tick)
    from bui import host
    host.run()
