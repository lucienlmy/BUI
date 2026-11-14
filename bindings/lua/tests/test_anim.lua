-- Animation: bui.gsap and bui.motion on kit widgets, headless, on a stepped clock
-- (gsap.ticker.tick): tweens, from/fromTo/set, controls, timeline positions, labels and
-- calls, stagger, springs, callbacks that fire once and are let go of, errors at the boundary.
-- SPDX-License-Identifier: GPL-2.0-or-later

local function check(cond, msg)
  if not cond then
    io.stderr:write("FAIL: " .. msg .. "\n")
    os.exit(1)
  end
end

local function near(a, b, eps)
  return math.abs(a - b) <= (eps or 1e-3)
end

local bui = require("bui")
local gsap, motion = bui.gsap, bui.motion
check(gsap and motion, "bui.gsap / bui.motion missing")
if not gsap.available then
  io.write("SKIP: test_anim.lua (no animation in this build)\n")
  os.exit(0)
end

-- our clock: the kit's frames don't tick it
gsap.ticker.useHostClock(true)
gsap.ticker.lagSmoothing(0)
local now = 100.0
local function step(s)
  now = now + s
  gsap.ticker.tick(now)
end
step(0)

local function slider(v)
  local s = bui.Node("slider")
  s:set("min", 0)
  s:set("max", 100)
  s:set("value", v or 0)
  return s
end

-- gsap.to: a slider's value, linear, 1 s
local a = slider(10)
local t = gsap.to(a, {value = 90, duration = 1, ease = "none"})
step(0.5)
check(near(a:get("value"), 50), "to: half way " .. a:get("value"))
check(t:isActive(), "to: active")
check(near(t:progress(), 0.5), "progress()")
t:pause()
step(0.25)
check(near(a:get("value"), 50), "pause holds")
t:resume()
step(0.25)
check(near(a:get("value"), 70), "resume goes on " .. a:get("value"))
t:seek(0.1)
check(near(a:get("value"), 18), "seek " .. a:get("value"))
t:progress(1)
check(near(a:get("value"), 90), "progress(1)")
t:reverse()
check(t:reversed(), "reversed()")
t:timeScale(2)
check(near(t:timeScale(), 2), "timeScale()")
step(0.25)
check(near(a:get("value"), 50), "reverse at 2x " .. a:get("value"))
t:kill()
check(not t:isActive(), "kill")
t = nil

-- relative values, string values, eases
local b = slider(10)
gsap.to(b, {value = "+=30", duration = 1, ease = "power2.out"})
step(0.5)
check(near(b:get("value"), 10 + 30 * 0.875), "power2.out (cubic), +=30 " .. b:get("value"))
step(0.5)
check(near(b:get("value"), 40), "relative end")

-- from: rendered at once, back to where it was; fromTo; set
local c = slider(20)
gsap.from(c, {value = 80, duration = 1, ease = "none"})
check(near(c:get("value"), 80), "from renders at once")
step(1)
check(near(c:get("value"), 20), "from ends where it was")
gsap.fromTo(c, {value = 0}, {value = 100, duration = 1, ease = "none"})
check(near(c:get("value"), 0), "fromTo starts at from")
step(0.25)
check(near(c:get("value"), 25), "fromTo quarter")
gsap.set(c, {value = 42})
check(near(c:get("value"), 42), "set")

-- colors: a frame's background, a swatch's color
local f = bui.Node("frame")
f:set("bg", {0, 0, 0, 1})
gsap.to(f, {backgroundColor = "#ffffff", duration = 1, ease = "none"})
step(0.5)
local r = f:get_color("bg")
check(near(r, 0.5), "backgroundColor half way " .. tostring(r))
local sw = bui.Node("swatch")
sw:set("color", {1, 0, 0, 1})
gsap.to(sw, {color = {0, 0, 1}, duration = 1, ease = "none"})
step(0.5)
local sr, _, sb = sw:get_color("color")
check(near(sr, 0.5) and near(sb, 0.5), "swatch color")
step(1)

-- repeat and yoyo (repeat_ for Lua's keyword, or ["repeat"])
local y = slider(0)
gsap.to(y, {value = 100, duration = 1, ease = "none", repeat_ = 1, yoyo = true})
step(1.5)
check(near(y:get("value"), 50), "yoyo back " .. y:get("value"))
step(0.5)
check(near(y:get("value"), 0), "yoyo end")

-- callbacks: fire once, and the motion lets go of them when it is freed
local done, updates = 0, 0
local weak = setmetatable({}, {__mode = "v"})
local function start() -- in a function: nothing on the caller's stack holds them
  local cb = function() done = done + 1 end
  weak[1] = cb
  weak[2] = gsap.to(slider(0), {value = 1, duration = 0.5, onComplete = cb,
                                on_update = function() updates = updates + 1 end})
end
start()
step(0.25)
collectgarbage()
collectgarbage()
check(weak[1] ~= nil, "a running motion holds its callback (its handle may be gone)")
step(0.25)
step(0.5)
check(done == 1, "onComplete once: " .. done)
check(updates >= 2, "onUpdate each frame: " .. updates)
collectgarbage()
collectgarbage()
check(weak[2] == nil, "the tween handle is collected")
check(weak[1] == nil, "the callback's ref is released")

-- timeline: positions, labels, calls (and their refs), defaults
local p, q, s = slider(0), slider(0), slider(0)
local calls = 0
local tl = gsap.timeline({defaults = {duration = 1, ease = "none"}})
tl:to(p, {value = 100})
  :to(q, {value = 100}, "<0.5")
  :addLabel("mid", ">")
  :to(s, {value = 100}, "mid+=0.25")
  :call(function(n) calls = calls + n end, {1}, "mid")
check(near(tl:labelTime("mid"), 1.5), "label at 1.5")
check(near(tl:duration(), 2.75), "timeline duration " .. tl:duration())
step(0.75)
check(near(p:get("value"), 75) and near(q:get("value"), 25) and s:get("value") == 0,
      "timeline at 0.75")
step(1.0)
check(near(p:get("value"), 100) and near(q:get("value"), 100) and calls == 1, "past the call")
tl:seek(0.25)
check(near(p:get("value"), 25) and near(q:get("value"), 0), "timeline seek back")
tl:progress(1)
check(near(s:get("value"), 100), "timeline progress(1)")
tl:kill()
tl = nil
collectgarbage()

-- stagger: one tween of many targets
local list = {slider(0), slider(0), slider(0)}
local st = gsap.to(list, {value = 100, duration = 1, ease = "none", stagger = 0.5})
step(0.5)
check(near(list[1]:get("value"), 50) and near(list[2]:get("value"), 0), "stagger 0.5")
check(near(st:duration(), 2), "stagger duration " .. st:duration())
step(2)
check(near(list[3]:get("value"), 100), "stagger end")

-- delayedCall, killTweensOf
local delayed = 0
gsap.delayedCall(0.5, function() delayed = delayed + 1 end)
step(0.25)
check(delayed == 0, "delayedCall waits")
step(0.5)
check(delayed == 1, "delayedCall fires")
local k = slider(0)
gsap.to(k, {value = 100, duration = 1, ease = "none"})
step(0.5)
gsap.killTweensOf(k)
step(0.5)
check(near(k:get("value"), 50), "killTweensOf stops it")

-- motion.animate: a tween (Motion's defaults), keyframes with times, a spring
local m1 = slider(0)
local ctl = motion.animate(m1, {value = 100}, {duration = 1, ease = "linear"})
step(0.5)
check(near(m1:get("value"), 50), "animate tween")
ctl:complete()
check(near(m1:get("value"), 100), "complete()")
local m2 = slider(0)
motion.animate(m2, {value = {0, 100, 0}}, {duration = 1, times = {0, 0.2, 1}, ease = {"linear", "linear"}})
step(0.1)
check(near(m2:get("value"), 50, 1e-2), "keyframes " .. m2:get("value"))
step(0.1)
check(near(m2:get("value"), 100, 1e-2), "keyframe peak")
local m3 = slider(0)
motion.animate(m3, {value = 100}, {type = "spring", stiffness = 300, damping = 20})
for _ = 1, 240 do step(1 / 60) end
check(near(m3:get("value"), 100, 0.6), "spring settles " .. m3:get("value"))
local m4 = {slider(0), slider(0)}
motion.animate(m4, {value = 10}, {duration = 1, ease = "linear", delay = motion.stagger(0.5, {from = "last"})})
step(0.5)
check(near(m4[2]:get("value"), 5) and m4[1]:get("value") == 0, "motion.stagger from last")
step(1)

-- eases by every family's names
local e = gsap.parseEase("power1.out")
check(near(e(0.5), 0.75), "parseEase")
check(near(gsap.parseEase("cubic-bezier(0.42, 0, 0.58, 1)")(0.5), 0.5), "css bezier")
check(near(gsap.parseEase({0.42, 0, 0.58, 1})(0.5), 0.5), "Motion's bezier array")
gsap.registerEase("myLinear", "none")
check(near(gsap.parseEase("myLinear")(0.3), 0.3), "registerEase")

-- errors at the boundary, naming what is wrong
local function fails(fn, pattern)
  local ok, err = pcall(fn)
  check(not ok, "should fail: " .. pattern)
  check(tostring(err):find(pattern, 1, true), "error '" .. tostring(err) .. "' lacks '" .. pattern .. "'")
end
local z = slider(0)
fails(function() gsap.to(z, {value = 1, ease = "power2.sideways"}) end, "unknown ease 'power2.sideways'")
fails(function() gsap.to(z, {valeu = 1}) end, "no property 'valeu'")
fails(function() gsap.to(z, {x = 100}) end, "no transforms or opacity ('x')")
fails(function() gsap.to(z, {value = "#ff0000"}) end, "takes a number")
fails(function() gsap.to(z, {value = 1, duration = "long"}) end, "'duration' takes a number")
fails(function() gsap.to(z, {value = 1, onComplete = 5}) end, "takes a function")
fails(function() gsap.to({}, {value = 1}) end, "no targets")
fails(function() gsap.to("#box", {value = 1}) end, "targets are bui nodes")
fails(function() gsap.to(z, {duration = 1}) end, "nothing to animate")
fails(function() motion.animate(z, {value = 1}, {stiffnes = 1}) end, "unknown option 'stiffnes'")
fails(function() gsap.timeline({ease = "none"}) end, "doesn't go in a timeline's vars")
fails(function() motion.animate(z, {value = {0, 1}}, {times = {0, 0.5, 1}}) end, "3 times for 2 keyframes")
local freed = slider(0)
freed:free()
fails(function() gsap.to(freed, {value = 1}) end, "freed node")

-- a node freed while it animates stops its motions (nothing writes to freed memory)
local gone = slider(0)
local gt = gsap.to(gone, {value = 100, duration = 1})
gone:free()
check(not gt:isActive(), "freeing the node stops its tween")
step(1)

-- numbers and positions are validated once, by the engine: NaN and infinity are errors, a
-- negative duration is 0, a repeat of any size is the engine's clamp, a killed motion stays killed
local nan, inf = 0 / 0, math.huge
local n1 = slider(0)
fails(function() gsap.to(n1, {value = 1, duration = nan}) end, "finite")
fails(function() gsap.to(n1, {value = 1, delay = nan}) end, "finite")
fails(function() gsap.to(n1, {value = 1, repeatDelay = nan}) end, "finite")
fails(function() gsap.to(n1, {value = 1, duration = inf}) end, "finite")
local neg = gsap.to(n1, {value = 50, duration = -1, ease = "none"})
step(0.1)
check(near(n1:get("value"), 50) and neg:duration() == 0, "a negative duration is 0")
for _, r in ipairs({2 ^ 31, 2 ^ 32, 2 ^ 70, 1e30}) do
  local rt = gsap.to(slider(0), {value = 1, duration = 1, ease = "none", repeat_ = r})
  check(rt:totalDuration() == 1000001, "repeat " .. r .. " is the engine's clamp: " .. rt:totalDuration())
  rt:kill()
end
local forever = gsap.to(slider(0), {value = 1, duration = 1, ease = "none", repeat_ = -1})
check(forever:totalDuration() > 1e100, "repeat -1 is for ever")
forever:kill()
local nothing = gsap.to(slider(0), {value = 1, duration = 0, repeat_ = -1})
step(0.1)
check(not nothing:isActive(), "a repeat for ever of nothing ends")
local cm = gsap.to(slider(0), {value = 1, duration = 1, ease = "none"})
fails(function() cm:seek(nan) end, "finite")
fails(function() cm:seek(inf) end, "finite")
fails(function() cm:progress(nan) end, "finite")
fails(function() cm:timeScale(nan) end, "finite")
fails(function() cm:totalProgress(inf) end, "finite")
cm:kill()
cm:play():resume():seek(0.5)
step(1)
check(not cm:isActive(), "a killed motion stays killed")
local ptl = gsap.timeline()
ptl:to(slider(0), {value = 1, duration = 2, ease = "none"})
for _, bad in ipairs({"<=1", "a b", "-inf", "0x10", "+=", "1e999", "<x", "a++1", "1 2"}) do
  fails(function() ptl:addLabel("l", bad) end, "bad position")
  fails(function() ptl:to(slider(0), {value = 1}, bad) end, "bad position")
  fails(function() ptl:call(function() end, bad) end, "bad position")
end
check(ptl:labelTime("l") == nil and near(ptl:duration(), 2), "a refused position changes nothing")
for _, c in ipairs({{"<", 0}, {">", 2}, {"<0.5", 0.5}, {">-0.5", 1.5}, {"+=1", 3}, {"-=1", 1}, {"50%", 1}, {0.25, 0.25}}) do
  ptl:addLabel("g", c[1])
  check(near(ptl:labelTime("g"), c[2]), "position " .. tostring(c[1]) .. " is " .. tostring(ptl:labelTime("g")))
end
ptl:kill()

io.write("OK: test_anim.lua\n")
os.exit(0)
