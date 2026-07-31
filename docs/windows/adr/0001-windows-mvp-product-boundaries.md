# ADR 0001: Windows MVP product boundaries

- Status: Accepted
- Date: 2026-07-31
- Owner: Product and engineering
- Applies to: Technical MVP, P0-alpha, P0-release

## Decision

- The minimum target is Windows 10 22H2 Build 19045 on x64.
- Windows 11 support remains a separate product decision. P0 does not use
  Windows 11-only UI or system integration.
- Technical MVP is an architecture proof, not a release.
- P0-alpha is an internal non-AI editing loop and cannot be presented as the
  complete Windows product.
- P0-release is the first releasable Windows MVP and includes the approved
  account, cloud transcription, generation writeback, signing, and release
  gates.
- P1 retains local AI, multicam and synchronization, ProRes/HDR, full Lottie
  parity, the in-app Agent, the remaining MCP surface, and automatic updates.
- P0 recognizes and preserves Lottie data. If a visible Lottie asset cannot be
  rendered, export preflight must block or require an explicit resolution.
- Missing fonts retain the authored font name. Substitution is reported in the
  editor and must be confirmed before export.
- The editor requires Direct3D 11 Feature Level 11_0. Hardware codecs are
  acceleration paths; software decode and encode remain required fallbacks.

## Consequences

- Scope labels must appear in plans, issues, and release evidence.
- A passing Technical MVP or P0-alpha does not authorize public release.
- This ADR does not lock Qt, FFmpeg, compiler, installer, or dependency
  versions.

## Revisit when

- Windows 10 dependency support becomes infeasible.
- Product scope moves a P1 capability into P0.
- The hardware or codec prototype cannot meet the minimum fallback behavior.

