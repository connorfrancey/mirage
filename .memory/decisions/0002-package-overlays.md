# 0002 Redirect packages before engine open

- **Date and status.** 2026-11-14. Accepted, recorded retrospectively from the overlay implementation and first-level-load fix (`3267e71`).
- **Context.** Supported content variations need to affect engine loads. UE3 holds package readers open across parts of a level session.
- **Decision.** Build derived packages and publish file redirects before UE3 opens the original path. Keep the chosen package stable while the engine holds it.
- **Reasoning.** The texture self-test records hangs when a package that the engine already opened was switched. Early redirection also covers first-level loads.
- **Alternatives.** Switching redirects for already-open files is the observed failure case. Editing the installed originals would change the source input and complicate restoration.
- **Consequences.** Verify redirect registration, package-open timing, and when a level or process restart is needed. Preserve stock package bytes and keep derived work in the existing cache.

[Texture overlays](../../loader/src/runtime/texture_overlays.cpp), [in-game self-test](../../loader/src/selftest.cpp), [asset paths](../assets.md).
