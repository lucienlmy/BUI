//! `Option<T: IntoElement>` as a child: `Some` renders, `None` renders nothing
//! and leaves its siblings in place (builder API and `rsx!`).

use blenderui::prelude::*;

struct View {
    on: bool,
}

impl Render for View {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let maybe_title: Option<&'static str> = self.on.then_some("title");
        let none_title: Option<String> = None;
        rsx! {
            <body style="margin: 0">
                <div id="first" class="h-4"></div>
                {self.on.then(|| div().id("cond").h(px(10.0)))}
                {maybe_title}
                {none_title}
                <div id="last" class="h-4"></div>
            </body>
        }
    }
}

struct Built {
    on: bool,
}

impl Render for Built {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        body().m_0()
            .child(div().id("first").h(px(16.0)))
            .child(self.on.then(|| div().id("cond").h(px(10.0))))
            .child(None::<Div>)
            .child(div().id("last").h(px(16.0)))
    }
}

fn open<V: Render>(cx: &mut App, v: V) -> WindowId {
    let opts = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds {
            origin: point(px(0.0), px(0.0)),
            size: size(px(200.0), px(200.0)),
        })),
        ..Default::default()
    };
    let id = cx.open_window(opts, |_, cx| cx.new(|_| v)).unwrap().id;
    cx.draw(id);
    id
}

#[test]
fn none_renders_nothing_and_some_renders() {
    Application::headless().run(|cx| {
        for built in [false, true] {
            let (shown, hidden) = if built {
                (open(cx, Built { on: true }), open(cx, Built { on: false }))
            } else {
                (open(cx, View { on: true }), open(cx, View { on: false }))
            };
            let y = |cx: &mut App, id: WindowId, n: &str| cx.element_bounds(id, n).map(|b| b.origin.y);
            assert_eq!(y(cx, shown, "cond"), Some(16.0), "Some renders after its sibling");
            assert_eq!(y(cx, hidden, "cond"), None, "None renders nothing");
            let last_shown = y(cx, shown, "last").unwrap();
            let last_hidden = y(cx, hidden, "last").unwrap();
            assert_eq!(last_hidden, 16.0, "None leaves siblings in place (no gap)");
            if built {
                assert_eq!(last_shown, 26.0, "Some pushes the next sibling down by its height");
            }
        }
    });
}
