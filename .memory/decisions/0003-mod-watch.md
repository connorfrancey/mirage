# 0003 Separate mod watching from lifecycle

- **Date and status.** 2026-11-22. Accepted, recorded retrospectively from `04e3e92`.
- **Context.** Installed-mod discovery, file snapshots, and runtime replacement run at different times and have different side effects.
- **Decision.** Keep discovery and file watching outside the lifecycle implementation. Lifecycle code consumes change notifications and owns reload, teardown, and re-registration.
- **Reasoning.** File observations can be collected independently. Mod callbacks and engine-owned work keep the lifecycle's ordering and thread constraints.
- **Alternatives.** Retrospective comparison. A watcher that calls mod callbacks directly would need to reproduce the lifecycle's thread dispatch and teardown rules.
- **Consequences.** Add watched paths and fingerprinting at the watcher boundary. Evaluate reload safety and run the result through lifecycle code.

[Watcher](../../loader/src/runtime/mod_watch.cpp), [lifecycle](../../loader/src/runtime/mods.cpp), [mod system](../mod-system.md).
