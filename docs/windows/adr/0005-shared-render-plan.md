# ADR 0005: Share preview and export rendering semantics

- Status: Accepted invariant; implementation proposed
- Date: 2026-07-31
- Owner: Media and rendering
- Applies to: Technical MVP and later

## Decision

Preview and export consume the same platform-neutral render plan, exact frame
mapping, text layout decisions, effect definitions, blend ordering, alpha
semantics, and shader math. Preview may reduce resolution or sampling quality,
but that degradation never changes the project or final export.

FFmpeg owns media probing, decode, encode, and container work. It does not
become a second implementation of timeline, text, effect, or layout rules.

## Required proof

The Technical MVP compares selected frames from preview and export using fixed
media and an approved exact or perceptual threshold. It also compares alpha,
color metadata, duration, and audio timing where applicable.

## Open work

- Define the render plan structure.
- Define HLSL and CPU fallback ownership.
- Choose golden media and comparison thresholds.
- Verify NVIDIA, AMD, Intel, and software fallback behavior.

