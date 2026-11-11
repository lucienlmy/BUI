//! `<video>`: the kit's player (`BK_video_*`, kit/intern/image/video.c), the
//! same one every binding uses. The OS decodes (Media Foundation,
//! AVFoundation, GStreamer) at the size drawn, frames update one GPU texture
//! in place, and nothing is decoded while the video isn't on screen.
//!
//! ```ignore
//! let clip = Video::new("intro.mp4").unwrap();
//! clip.set_loop(true);
//! clip.on_state(|s| if s == VideoState::Ended { println!("done") });
//! div().child(video().video(clip.clone()).w(px(640.0)));
//! // or let the element own it: <video src="intro.mp4" loop muted poster="intro.jpg" />
//! ```

use std::collections::HashMap;
use std::ffi::CString;
use std::ptr::NonNull;
use std::rc::Rc;
use std::time::Duration;

use crate::sys;

/// What a video is doing (`BK_VIDEO_*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoState {
    /// Not decoded yet (it loads when first drawn).
    Loading,
    Playing,
    Paused,
    /// The last frame shows (not looping).
    Ended,
    /// No backend reads the file (or it is not a video).
    Error,
}

impl VideoState {
    const fn from_raw(s: i32) -> Self {
        match s {
            sys::BK_VIDEO_LOADING => Self::Loading,
            sys::BK_VIDEO_PLAYING => Self::Playing,
            sys::BK_VIDEO_PAUSED => Self::Paused,
            sys::BK_VIDEO_ENDED => Self::Ended,
            _ => Self::Error,
        }
    }
}

type Listener = Box<dyn FnMut(VideoState)>;

/// A video and its controls. UI thread only (`!Send`); share it with `Rc`.
/// Autoplays with sound (`set_muted(true)` first for a silent one); the frames
/// follow the sound, which plays on while the video is off screen.
pub struct Video {
    ptr: NonNull<sys::BKVideo>,
    /// `on_state`'s callback, boxed twice so the kit holds a thin pointer.
    listener: Option<Box<Listener>>,
}

unsafe extern "C" fn heard(_v: *mut sys::BKVideo, state: i32, data: *mut std::ffi::c_void) {
    // SAFETY: `data` is the `Box<Listener>` this video owns (see `on_state`),
    // alive until the video is dropped, which clears the listener first.
    let f = unsafe { &mut *data.cast::<Listener>() };
    f(VideoState::from_raw(state));
}

impl Video {
    /// A video file. `None` for an empty path or one with a NUL byte.
    pub fn new(src: &str) -> Option<Self> {
        let path = CString::new(src).ok()?;
        // SAFETY: `path` is NUL-terminated for the call (the kit copies it).
        let ptr = NonNull::new(unsafe { sys::BK_video_new(path.as_ptr()) })?;
        Some(Self { ptr, listener: None })
    }

    pub(crate) const fn as_ptr(&self) -> *mut sys::BKVideo {
        self.ptr.as_ptr()
    }

    pub fn play(&self) {
        // SAFETY: a live video (all methods below likewise).
        unsafe { sys::BK_video_play(self.as_ptr()) };
    }

    pub fn pause(&self) {
        unsafe { sys::BK_video_pause(self.as_ptr()) };
    }

    /// The frame at `t` shows, paused or not.
    pub fn seek(&self, t: Duration) {
        unsafe { sys::BK_video_seek(self.as_ptr(), t.as_secs_f64()) };
    }

    /// Playback speed (1/16 .. 16).
    pub fn set_rate(&self, rate: f64) {
        unsafe { sys::BK_video_set_rate(self.as_ptr(), rate) };
    }

    pub fn set_loop(&self, on: bool) {
        unsafe { sys::BK_video_set_loop(self.as_ptr(), i32::from(on)) };
    }

    /// Silent (the frames still follow the sound's clock).
    pub fn set_muted(&self, muted: bool) {
        unsafe { sys::BK_video_set_muted(self.as_ptr(), i32::from(muted)) };
    }

    pub fn muted(&self) -> bool {
        unsafe { sys::BK_video_muted(self.as_ptr()) != 0 }
    }

    /// Keep the system's audio effects. Windows plays RAW, without them (Realtek, Dolby and
    /// Nahimic "enhancements"), which opens the output in half the time; turn them on for a video
    /// that should have them. Other systems ignore it. Takes effect when the sound opens, or at
    /// once if it already did (the output opens again).
    pub fn set_audio_effects(&self, on: bool) {
        unsafe { sys::BK_video_set_audio_effects(self.as_ptr(), i32::from(on)) };
    }

    pub fn audio_effects(&self) -> bool {
        unsafe { sys::BK_video_audio_effects(self.as_ptr()) != 0 }
    }

    /// The sound's volume, 0.0 ..= 1.0.
    pub fn set_volume(&self, volume: f64) {
        unsafe { sys::BK_video_set_volume(self.as_ptr(), volume) };
    }

    pub fn volume(&self) -> f64 {
        unsafe { sys::BK_video_volume(self.as_ptr()) }
    }

    /// Its sound plays: the file has an audio track this system decodes, and
    /// there is an output device (`false` until it has started).
    pub fn has_audio(&self) -> bool {
        unsafe { sys::BK_video_has_audio(self.as_ptr()) != 0 }
    }

    /// An image file shown until the first frame.
    pub fn set_poster(&self, src: &str) {
        if let Ok(path) = CString::new(src) {
            unsafe { sys::BK_video_set_poster(self.as_ptr(), path.as_ptr()) };
        }
    }

    pub fn state(&self) -> VideoState {
        VideoState::from_raw(unsafe { sys::BK_video_state(self.as_ptr()) })
    }

    /// Where it is (the frame shown).
    pub fn time(&self) -> Duration {
        Duration::from_secs_f64(unsafe { sys::BK_video_time(self.as_ptr()) }.max(0.0))
    }

    /// `None` until loaded (or when the file doesn't say).
    pub fn duration(&self) -> Option<Duration> {
        let d = unsafe { sys::BK_video_duration(self.as_ptr()) };
        (d > 0.0).then(|| Duration::from_secs_f64(d))
    }

    /// Intrinsic size, once loaded (or the poster's).
    pub fn size(&self) -> Option<(u32, u32)> {
        let (mut w, mut h) = (0, 0);
        let ok = unsafe { sys::BK_video_size(self.as_ptr(), &raw mut w, &raw mut h) } != 0;
        (ok && w > 0 && h > 0).then_some((w as u32, h as u32))
    }

    /// Called on every state change (loaded, playing, paused, ended, error).
    pub fn on_state(&mut self, f: impl FnMut(VideoState) + 'static) {
        let mut boxed: Box<Listener> = Box::new(Box::new(f));
        let data: *mut Listener = &raw mut *boxed;
        // SAFETY: the box stays at this address while `self` holds it.
        unsafe { sys::BK_video_set_listener(self.as_ptr(), Some(heard), data.cast()) };
        self.listener = Some(boxed);
    }

    /// The current frame in a BUI texture for Dear ImGui (`0`: make one; it is
    /// updated in place after). Calling it each frame keeps the video playing.
    pub fn texture(&self, tex: i32, max_w: u32, max_h: u32) -> i32 {
        unsafe { sys::BK_video_texture(self.as_ptr(), tex, max_w as i32, max_h as i32) }
    }

    /// The video backends this build has (`"mf"`, `"gstreamer"`, ...).
    pub fn backends() -> &'static str {
        // SAFETY: a static NUL-terminated string.
        unsafe { std::ffi::CStr::from_ptr(sys::BK_video_backends()) }.to_str().unwrap_or("")
    }
}

impl Drop for Video {
    fn drop(&mut self) {
        // SAFETY: ours; the kit drops the listener with it (no call after this).
        unsafe {
            sys::BK_video_set_listener(self.as_ptr(), None, std::ptr::null_mut());
            sys::BK_video_free(self.as_ptr());
        }
    }
}

impl std::fmt::Debug for Video {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Video").field("state", &self.state()).finish_non_exhaustive()
    }
}

/// `<video src>` elements' players, kept by the window by `src` (one player
/// per file and window) with the attributes last applied, so a re-render
/// changes the player only when an attribute did.
#[derive(Default)]
pub(crate) struct VideoCache {
    players: HashMap<String, (Rc<Video>, VideoAttrs)>,
}

#[derive(Clone, Default, PartialEq, Eq)]
pub(crate) struct VideoAttrs {
    pub autoplay: bool,
    pub looped: bool,
    pub muted: bool,
    pub audio_effects: bool,
    pub poster: Option<String>,
}

impl VideoCache {
    pub(crate) fn get(&mut self, src: &str, attrs: VideoAttrs) -> Option<Rc<Video>> {
        if let Some((v, had)) = self.players.get_mut(src) {
            if *had != attrs {
                apply(v, had, &attrs);
                *had = attrs;
            }
            return Some(v.clone());
        }
        let v = Rc::new(Video::new(src)?);
        apply(&v, &VideoAttrs { autoplay: true, ..VideoAttrs::default() }, &attrs);
        self.players.insert(src.to_owned(), (v.clone(), attrs));
        Some(v)
    }
}

fn apply(v: &Video, had: &VideoAttrs, now: &VideoAttrs) {
    if had.autoplay != now.autoplay {
        if now.autoplay { v.play() } else { v.pause() }
    }
    if had.looped != now.looped {
        v.set_loop(now.looped);
    }
    if had.muted != now.muted {
        v.set_muted(now.muted);
    }
    if had.audio_effects != now.audio_effects {
        v.set_audio_effects(now.audio_effects);
    }
    if had.poster != now.poster {
        v.set_poster(now.poster.as_deref().unwrap_or(""));
    }
}
