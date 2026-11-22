# Mirror’s Edge Mod Identity Standard 1.0

This proposed standard lets launchers, mod managers, patchers, and asset tools identify releases for Mirror’s Edge (2008, PC). It applies to gameplay mods, maps, texture packs, executable patches, and other distributions that change the game. Conforming releases carry one machine-readable identity record. A conforming tool can index a release without running it or installing it.

**MUST**, **SHOULD**, and **MAY** describe requirements, recommendations, and permitted options.

## Discovery

The identity record is a UTF-8 JSON object, at most 65,536 bytes, without a byte-order mark. Object member names MUST be unique, including in nested objects and after decoding JSON escapes. Readers MUST reject malformed UTF-8, unpaired surrogates, and malformed JSON. The filename, when applicable, is exactly `me-mod.json`.

| Distribution | Identity record location |
| --- | --- |
| Loose folder | `me-mod.json` at the mod root |
| ZIP | `me-mod.json` at the archive root, or `<folder>/me-mod.json` if every file belongs to that single top-level folder |
| Standalone file or other container | A sidecar alongside it, named by appending `.me-mod.json` to the entire artifact filename, for example `patch.exe.me-mod.json` |
| MirageMod (`.miragemod`) | A sidecar next to the package, for example `example.miragemod.me-mod.json` |

A scanner checks immediate child folders and supported package files in each configured mod directory. It reads each package’s index before looking for the manifest. It MUST reject ambiguous records, including duplicate ZIP names and case variants of the identity record in an otherwise valid identity location. Discovery MUST avoid following symlinks or reparse points to read manifests and MUST limit the number of bytes read or decompressed for each record. Installed mod-management tools SHOULD keep a copy of the record alongside their installation receipt so they can identify releases after their files have been copied into shared game directories.

## Required fields

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | string | `mirrors-edge-mod/1.0` |
| `game` | string | `mirrors-edge-2008-pc` |
| `id` | string | Publisher-scoped, permanent mod identifier |
| `name` | string | Human-readable display name, 1–128 Unicode scalar values with no control characters or leading or trailing ASCII whitespace |
| `version` | string | Release version in SemVer 2.0.0 syntax, at most 128 ASCII characters |

The ID consists of at least three dot-separated labels. Each label consists of 1–63 lowercase ASCII letters, digits, or hyphens and starts and ends with a letter or digit. The full ID is at most 191 bytes. A publisher SHOULD use a reversed domain it controls, followed by a mod slug, such as `com.example.wallrun-trails`. A publisher without a domain MAY use `community.<publisher>.<mod>`. Community registries SHOULD check publisher namespace ownership before accepting listings. Comparisons use exact ASCII bytes.

Publishers MUST keep the ID across renames, updates, and changes to distribution format. A fork distributed independently MUST use a new ID. Publishers MUST assign a new version when changing the content of a published artifact. SemVer build metadata participates in release identity and is ignored for precedence. If versions with different build metadata have equal precedence, an updater MUST treat them as tied rather than infer an order.

The mod identity key is `(game, id)`. The release key is `(game, id, version)`. File names, folder names, display names, and download locations can change independently of these keys.

## Optional fields

| Field | Type | Meaning |
| --- | --- | --- |
| `variant` | string | Artifact selector, for example `mirage` or `manual`. Uses the same grammar as a single ID label. Defaults to `default`. |
| `requires` | object | Mod IDs mapped to version constraints. Every constraint must match an installed or selected release for the same game. |
| `optional` | object | Mod IDs mapped to version constraints for optional integrations. These constraints apply only to the integration. |
| `conflicts` | object | Mod IDs mapped to version constraints for releases that must not be active alongside this mod. |
| `legacy` | object | Tool-specific aliases. Version 1.0 defines `mirage`, the ID used in `mod.toml`. |

The artifact key is `(game, id, version, variant)`. Publishers SHOULD use variants for alternative distributions of one release and preserve the same variant label across updates for the same distribution kind. Tools SHOULD group variants and allow one active variant per mod. When two packages claim the same artifact key, tools SHOULD report both sources. They MAY deduplicate packages whose content they have independently verified to be identical.

Relationship keys use the same grammar as `id`. A relationship MUST NOT refer to the record’s own ID, and the same ID MUST NOT appear in multiple relationship objects. A version constraint is `*` (any valid version), or one or more comparisons separated by single ASCII spaces and combined with AND. Each comparison comprises one of `=`, `<`, `<=`, `>`, or `>=` immediately followed by a complete semantic version. For example, `>=1.2.0 <2.0.0`. Comparisons use SemVer precedence, including prerelease ordering and ignoring build metadata. Prereleases satisfy constraints by the same rules as other versions. When the tools are only cataloging mods, they can display unresolved relationships without changing any installation.

`legacy.mirage` MUST contain 2–64 lowercase ASCII letters, digits, hyphens, or underscores. When a package includes `mod.toml` and declares this alias, the alias MUST equal the TOML `id`. When publishing both records, the two versions MUST have the same text. `mod.toml` continues to own Mirage entrypoints, settings, and runtime dependency declarations. Other tools use the shared IDs from `me-mod.json`. Publishers SHOULD keep both dependency declarations consistent when they describe the same dependency.

## Compatibility and trust

- Existing mods with only `mod.toml` can be indexed by their Mirage-local ID and version. Tools MUST label that identity as tool-local. An alias alone is insufficient to merge a legacy record into a publisher-scoped identity or to match a global dependency.
- Schema identifiers use `mirrors-edge-mod/<major>.<minor>`, with each version number written as an unsigned decimal integer of at most nine digits and no leading zeros except for zero itself. Tools MUST report unsupported schema major versions separately from invalid records and MUST avoid substituting legacy identity when a package carries an invalid or unsupported shared record.
- A compatible minor version MAY add optional fields while preserving the meaning of all existing fields. A reader MUST ignore unknown fields within a supported major version. Applications that round-trip records SHOULD preserve unknown fields. A breaking change requires a new major version.
- An identity record is a publisher claim. Tools MUST NOT use it as proof of authorship, package integrity, or permission to execute code. An untrusted package that claims a known ID MUST NOT silently replace an installed release based on the ID alone. A sidecar MAY be cataloged without opening the executable it describes, and its presence alone supplies no binding to the executable bytes.

## Example

A loose Mirage mod ships these files together. A ZIP may place all three under one top-level folder.

```text
wallrun-trails/
  me-mod.json
  mod.toml
  main.lua
```

`me-mod.json`

```json
{
  "schema": "mirrors-edge-mod/1.0",
  "game": "mirrors-edge-2008-pc",
  "id": "com.example.wallrun-trails",
  "name": "Wallrun Trails",
  "version": "1.2.0",
  "variant": "mirage",
  "optional": {
    "com.example.photo-tools": ">=2.0.0 <3.0.0"
  },
  "legacy": {
    "mirage": "wallrun-trails"
  }
}
```

`mod.toml`

```toml
id = "wallrun-trails"
name = "Wallrun Trails"
version = "1.2.0"
lua = "main.lua"
```

A manual distribution of this same release could carry the same game, ID, and version with `"variant": "manual"` and omit the Mirage alias. Both distributions appear under Wallrun Trails in a catalog. If Photo Tools 2.3.0 is present, it satisfies the optional relationship.
