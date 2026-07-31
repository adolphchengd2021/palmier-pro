# ADR 0005: Share preview and export rendering semantics

- Status: Accepted invariant; synthetic M1 reference spike implemented
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

## Current implementation

ADR 0009 defines the immutable v1 Technical MVP plan and its CPU/D3D11 WARP
reference renderers. It is not connected to a project compiler, decoded media,
preview presentation, or export encoding yet. It is internally self-consistent
but is not yet a pixel oracle for the shipping Swift BGRA8 compositor.

## Open work

- Compile supported project segments into the plan with hard refusal of every
  visible unsupported feature.
- Generate Swift BGRA8 goldens and freeze sampling, quantization, and clipping.
- Choose golden media and comparison thresholds.
- Verify NVIDIA, AMD, Intel, and software fallback behavior.
