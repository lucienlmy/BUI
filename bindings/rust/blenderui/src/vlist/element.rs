//! [`VariableList`]: the element that shows a [`ListState`].
//!
//! One painted leaf (a canvas drawn straight from the state) inside a
//! wrapper that takes pointer, wheel and keyboard input: a frame builds a
//! constant number of elements however many items there are. Layout (which
//! measures and mutates the state) runs while rendering, in the deferred
//! hook; painting only reads.

use std::rc::Rc;

use blenderui_vlist::{ChatStyle, Hit, ItemPainter, ItemSource, List, Measurer, VecSource, SCROLLBAR_WIDTH};

use super::state::{ListState, ViewInfo};
use crate::css::Decl;
use crate::element::{canvas_with, Div};
use crate::html::div;
use crate::{px, AnyElement, App, Entity, IntoElement, MouseButton, ParentElement, ScrollPhase, Styled, Window};

type Listener<S, P> = Rc<dyn Fn(usize, &mut List<S, P>)>;
type HitListener<S, P> = Rc<dyn Fn(usize, u32, &mut List<S, P>)>;
type HoverListener<S, P> = Rc<dyn Fn(Option<usize>, &mut List<S, P>)>;

/// The press handlers of a list.
struct Handlers<S: ItemSource, P: Measurer + ItemPainter> {
    click: Option<Listener<S, P>>,
    hit: Option<HitListener<S, P>>,
}

/// A variable-height list over `state`.
///
/// ```ignore
/// let list = cx.new(|cx| ListState::new(List::new(source, style, 600.0, 400.0, Options::default()).unwrap(), cx));
/// // in render:
/// div().size_full().child(variable_list(&list).on_item_click(|i, list| println!("{i}")))
/// ```
pub struct VariableList<S: ItemSource + 'static, P: Measurer + ItemPainter + 'static> {
    state: Entity<ListState<S, P>>,
    base: Div,
    on_item_click: Option<Listener<S, P>>,
    on_item_hit: Option<HitListener<S, P>>,
    on_view_change: Option<Rc<dyn Fn(&ViewInfo)>>,
    on_copy: Option<Rc<dyn Fn(&ListState<S, P>) -> Option<String>>>,
    on_item_hover: Option<HoverListener<S, P>>,
}

/// The element for a list state.
pub fn variable_list<S: ItemSource, P: Measurer + ItemPainter>(state: &Entity<ListState<S, P>>) -> VariableList<S, P> {
    VariableList { state: *state, base: div().relative().overflow_hidden().size_full(), on_item_click: None, on_item_hit: None, on_view_change: None, on_copy: None, on_item_hover: None }
}

/// A chat: a [`VariableList`] over a [`ChatList`] where a click selects a
/// whole message (click it again to deselect).
pub fn chat_list(state: &Entity<ListState<VecSource, ChatStyle>>) -> VariableList<VecSource, ChatStyle> {
    variable_list(state).on_item_click(|i, list| list.toggle_selected(i))
}

impl<S: ItemSource, P: Measurer + ItemPainter> VariableList<S, P> {
    /// The visible range or the count changed (TanStack Virtual's `onChange`):
    /// called after the frame that did it, with the list not borrowed.
    #[must_use]
    pub fn on_view_change(mut self, f: impl Fn(&ViewInfo) + 'static) -> Self {
        self.on_view_change = Some(Rc::new(f));
        self
    }

    /// An item was clicked (item index). The closure may change the list.
    #[must_use]
    pub fn on_item_click(mut self, f: impl Fn(usize, &mut List<S, P>) + 'static) -> Self {
        self.on_item_click = Some(Rc::new(f));
        self
    }

    /// An item was pressed: its index and the part of it that was hit, as
    /// [`ItemPainter::hit`] answers it (0: the item itself). Called after
    /// `on_item_click`, from the frame that is on screen. The closure may change the list.
    #[must_use]
    pub fn on_item_hit(mut self, f: impl Fn(usize, u32, &mut List<S, P>) + 'static) -> Self {
        self.on_item_hit = Some(Rc::new(f));
        self
    }

    /// The pointer moved onto another item, or left the list (`None`). The closure may change the list; only a repaint follows.
    #[must_use]
    pub fn on_item_hover(mut self, f: impl Fn(Option<usize>, &mut List<S, P>) + 'static) -> Self {
        self.on_item_hover = Some(Rc::new(f));
        self
    }

    /// Ctrl+C: the text to put on the clipboard (`None`: nothing is selected).
    #[must_use]
    pub fn on_copy(mut self, f: impl Fn(&ListState<S, P>) -> Option<String> + 'static) -> Self {
        self.on_copy = Some(Rc::new(f));
        self
    }
}

impl<S: ItemSource, P: Measurer + ItemPainter> Styled for VariableList<S, P> {
    fn decls_mut(&mut self) -> &mut Vec<Decl> {
        self.base.decls_mut()
    }
}

impl<S: ItemSource, P: Measurer + ItemPainter> IntoElement for VariableList<S, P> {
    fn into_any_element(self) -> AnyElement {
        let VariableList { state, base, on_item_click, on_item_hit, on_view_change, on_copy, on_item_hover } = self;
        let el = base.defer(move |window, cx, el| {
            let id = state.read(cx).id.clone();
            let known = window.bounds_of(&id);
            let bounds = known.unwrap_or_default();
            let scale = window.scale_factor();
            state.update(cx, |s, _| s.sync(bounds, scale, window));
            // this frame's placement is only known after layout: the list fits it then, before anything is
            // painted, and the range it paints is the one reported (outside the entity's borrow)
            let (after, after_id) = (state, id.clone());
            super::after_layout(Box::new(move |frame, window, cx| {
                let placed = frame.by_key().get(&crate::layout::id_key(&after_id)).map(|&i| frame.nodes[i].bounds);
                let scale = window.scale_factor();
                let view = after.update(cx, |s, _| {
                    if let Some(placed) = placed.filter(|p| s.bounds != *p) {
                        s.sync(placed, scale, window);
                    }
                    s.take_view_change()
                });
                if let (Some(f), Some(info)) = (&on_view_change, view) {
                    f(&info);
                }
            }));
            let (down, mv, up, wh, key, leave) = (state, state, state, state, state, state);
            let (hover_move, hover_leave) = (on_item_hover.clone(), on_item_hover);
            let handlers = Handlers { click: on_item_click, hit: on_item_hit };
            let taken = std::mem::replace(el, Div::new("div"));
            *el = taken
                .id(id)
                .on_any_mouse_down(move |e, w: &mut Window, cx: &mut App| {
                    if e.button != MouseButton::Left {
                        return;
                    }
                    let local = |s: &ListState<S, P>| (e.position.x.0 - s.bounds.origin.x, e.position.y.0 - s.bounds.origin.y);
                    state_pointer_down(down, &handlers, local, cx);
                    w.set_pointer_capture(cx);
                    w.refresh();
                })
                .on_mouse_move(move |e, w, cx| {
                    let (moved, hovered) = mv.update(cx, |s, _| {
                        let y = e.position.y.0 - s.bounds.origin.y;
                        if e.pressed_button.is_none() {
                            s.pointer_up();
                            let x = e.position.x.0 - s.bounds.origin.x;
                            let over = if x < s.bounds.size.width - SCROLLBAR_WIDTH { s.view.item_at(y) } else { None };
                            let changed = s.hover != over;
                            if changed {
                                s.hover = over;
                                if let Some(f) = &hover_move {
                                    f(over, s);
                                }
                            }
                            (false, changed)
                        } else {
                            (s.pointer_move(y), false)
                        }
                    });
                    if moved {
                        w.refresh();
                    } else if hovered {
                        w.repaint();
                    }
                })
                .on_mouse_up(MouseButton::Left, move |_, _, cx| up.update(cx, |s, _| s.pointer_up()))
                .on_hover(move |inside, w, cx| {
                    if !*inside {
                        let left = leave.update(cx, |s, _| {
                            let had = s.hover.take().is_some();
                            if had && let Some(f) = &hover_leave {
                                f(None, s);
                            }
                            had
                        });
                        if left {
                            w.repaint();
                        }
                    }
                })
                .on_scroll_wheel(move |e, w, cx| {
                    wh.update(cx, |s, _| {
                        // Blender's wheel scrolls at once, a notch or a touchpad fraction alike: no easing
                        // (unless `Options::smooth_wheel`, which eases a notch in, as macOS does).
                        // A touch drag moves the list exactly as far as the finger went, and past an end it
                        // stretches (`Options::overscroll`) until the finger lifts.
                        let by = e.pixels.map_or(f64::from(e.delta) * WHEEL_NOTCH_PX, f64::from);
                        match e.phase {
                            ScrollPhase::Drag => s.view.drag(-by),
                            ScrollPhase::Fling => s.view.fling(-by),
                            ScrollPhase::End => s.view.release(),
                            ScrollPhase::Wheel => {
                                let eased = e.pixels.is_none() && s.view.options().smooth_wheel;
                                s.view.wheel(-by, !eased);
                            }
                        }
                    });
                    w.request_animation_frame();
                })
                .on_key_down(move |e, w, cx| {
                    if e.keystroke.modifiers.control && e.keystroke.key.eq_ignore_ascii_case("c") {
                        if let Some(text) = on_copy.as_ref().and_then(|f| f(key.read(cx))) {
                            w.write_clipboard(&text);
                            // the list's text is the copy: an ancestor's Ctrl+C must not overwrite it
                            cx.stop_propagation();
                        }
                        return;
                    }
                    let handled = key.update(cx, |s, _| s.key(&e.keystroke.key, LINE_PX));
                    if handled {
                        w.request_animation_frame();
                    }
                })
                .child(canvas_with(move |b, p, cx| state.read(cx).draw(b, p, scale)).absolute().left(px(0.0)).top(px(0.0)).w_full().h_full());
        });
        AnyElement::Element(el)
    }
}

/// A line of text in px, for keyboard steps.
const LINE_PX: f64 = 19.0;

/// One wheel notch, in px: several messages per notch.
const WHEEL_NOTCH_PX: f64 = 57.0;

fn state_pointer_down<S: ItemSource, P: Measurer + ItemPainter>(
    state: Entity<ListState<S, P>>,
    handlers: &Handlers<S, P>,
    local: impl Fn(&ListState<S, P>) -> (f32, f32),
    cx: &mut App,
) {
    state.update(cx, |s, _| {
        let (x, y) = local(s);
        if let Hit::Item(i) = s.pointer_down(x, y) {
            if let Some(f) = &handlers.click {
                f(i, s);
            }
            if let Some(f) = &handlers.hit {
                let part = s.hit_part(i, x, y);
                f(i, part, s);
            }
        }
    });
}
