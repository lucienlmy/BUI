"""Animation demo (bui.gsap, bui.motion).

    python examples/animation_demo.py

Sliders ease and spring to their targets, a progress bar fills through a timeline, and a
swatch fades between colors. Widgets animate what BUI draws for them: values and colors.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bindings", "python"))

from bui import gsap, motion  # noqa: E402
from bui.components import App, Button, Column, ColorSwatch, Label, NumberSlider, ProgressBar  # noqa: E402


def build():
    eased = NumberSlider("Ease", 0, min=0, max=100)
    sprung = NumberSlider("Spring", 0, min=0, max=100)
    bar = ProgressBar(0.0)
    swatch = ColorSwatch("Tint", (0.28, 0.45, 0.70, 1.0))
    state = {"high": False}

    def play():
        state["high"] = not state["high"]
        to = 90 if state["high"] else 10
        gsap.to(eased, value=to, duration=0.8, ease="power2.out")
        # a real spring: it overshoots, settles, and hands its velocity on if interrupted
        motion.animate(sprung, {"value": to}, type="spring", stiffness=120, damping=8)
        tl = gsap.timeline(defaults={"duration": 0.5})
        tl.to(bar, factor=1.0, ease="power1.inOut").to(bar, factor=0.2, ease="power1.inOut")
        gsap.to(swatch, color=(0.91, 0.56, 0.24, 1.0) if state["high"] else (0.28, 0.45, 0.70, 1.0),
                duration=0.6)

    app = App("Animation", (360, 260),
              Column(Label("Press the button"), eased, sprung, bar, swatch, Button("Animate", play),
                     padding=8, spacing=6))
    # the sliders ease in when the window opens
    motion.mount(eased, initial={"value": 0}, animate={"value": 50}, transition={"duration": 0.6})
    return app


if __name__ == "__main__":
    build().run()
