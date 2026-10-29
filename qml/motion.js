.pragma library
// Motion tokens: damped springs (mass 1, stiffness k, damping c) shared by every surface,
// with the values of Pulse's motion prototype. A spring is a pure function of elapsed time,
// so a transition keeps its path however frames fall.
//
// Two ways to use one:
// - fixed transitions: `duration: Motion.pop.ms; easing.type: Easing.BezierSpline;
//   easing.bezierCurve: Motion.pop.curve` (the curve may overshoot 1, as the spring does);
// - followers that are retargeted while moving (hover and selection plates): Glide.qml
//   integrates k and c directly and keeps its velocity across targets.

// Position of a spring released at 0 towards 1 after ms milliseconds.
function progress(k, c, ms) {
    if (ms <= 0) return 0
    const t = ms / 1000, w0 = Math.sqrt(k), zeta = c / (2 * w0)
    if (zeta < 0.999) {
        const wd = w0 * Math.sqrt(1 - zeta * zeta), e = Math.exp(-zeta * w0 * t)
        return 1 - e * (Math.cos(wd * t) + (zeta * w0 / wd) * Math.sin(wd * t))
    }
    if (zeta <= 1.001) return 1 - Math.exp(-w0 * t) * (1 + w0 * t)
    const r = w0 * Math.sqrt(zeta * zeta - 1), r1 = -zeta * w0 + r, r2 = -zeta * w0 - r
    return 1 - (r2 * Math.exp(r1 * t) - r1 * Math.exp(r2 * t)) / (r2 - r1)
}

// Time after which the spring stays within 0.5% of its target.
function settleMs(k, c) {
    const w0 = Math.sqrt(k), zeta = c / (2 * w0)
    const decay = zeta < 1 ? zeta * w0 : w0 * (zeta - Math.sqrt(Math.max(0, zeta * zeta - 1)))
    return Math.round(Math.min(900, Math.max(60, 1000 * 5.3 / Math.max(decay, 1))))
}

// The spring over [0, ms] as an Easing.BezierSpline: cubic Hermite segments (value and
// slope matched at each joint), the last one landing exactly on (1, 1).
function curve(k, c, ms, segments) {
    const n = segments || 10, h = 0.5
    const f = u => progress(k, c, u * ms)
    const slope = u => (progress(k, c, u * ms + h) - progress(k, c, Math.max(0, u * ms - h))) / (u * ms + h - Math.max(0, u * ms - h)) * ms
    const points = []
    for (let i = 0; i < n; ++i) {
        const u0 = i / n, u1 = (i + 1) / n, d = u1 - u0
        const y0 = f(u0), y1 = i === n - 1 ? 1 : f(u1)
        const s0 = slope(u0), s1 = i === n - 1 ? 0 : slope(u1)
        points.push(u0 + d / 3, y0 + s0 * d / 3, u1 - d / 3, y1 - s1 * d / 3, u1, y1)
    }
    return points
}

function spring(k, c) {
    const ms = settleMs(k, c)
    return { k: k, c: c, ms: ms, curve: curve(k, c, ms) }
}

var snappy = spring(520, 34)   // buttons, small moves (~3% overshoot)
var smooth = spring(380, 36)   // reflow, rows sliding aside; no overshoot
var plate = spring(2600, 98)   // hover / selection plates: ~40 ms behind a sweeping pointer
var pop = spring(620, 30)      // things landing: menus, badges, new tabs (~9% overshoot)
var open = spring(480, 38)     // a surface growing from where it came from
var close = spring(700, 52)    // ...and shrinking back
var soft = spring(260, 24)     // something flying home

// The stretching selection pill: its leading edge on a stiff spring, the trailing one softer.
var pillLead = spring(900, 60)
var pillTrail = spring(300, 30)

// Plates fade in and out this fast; a pointer coming back mid-fade glides on from there.
var plateFadeInMs = 90
var plateFadeOutMs = 120

// A critically damped spring over t in [0, 1], normalised to land exactly on 1: no overshoot,
// half way at about a quarter of the time. Items changing view travel on it.
function morph(t) {
    t = Math.max(0, Math.min(1, t))
    const w = 7, norm = 1 - (1 + w) * Math.exp(-w)
    return Math.min(1, (1 - (1 + w * t) * Math.exp(-w * t)) / norm)
}

function smoothStep(edge0, edge1, x) {
    const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)))
    return t * t * (3 - 2 * t)
}
