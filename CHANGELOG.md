# Changelog

## 0.1.0-alpha.2 — 2026-11-02

### Added

- `init`, `run`, `check`, `test` and `version` command workflows.
- Recursive project test discovery with deterministic result summaries.
- Named arguments, `*args`, `**kwargs`, array/map spreading and trailing lambdas.
- Stable singleton-object and user-defined extension-function baselines.
- Generated data-class components, copy, equality, hash and text members for
  the documented primitive-property subset.
- A persistent todo CLI example and its multi-process integration test.
- Relocatable Windows ZIP packaging with the standard library, documentation
  and runnable examples.
- Windows Debug/Release CI with packaged build artifacts.

### Changed

- Public standard-library APIs no longer require direct VM primitives; explicit
  low-level access is isolated behind `loxis.unsafe`.
- CLI path and numeric-option failures now produce specific diagnostics instead
  of generic conversion failures.
- Data-class property diagnostics carry an exact source location.

### Fixed

- Capturing and nested closure state updates.
- Data-class generated members across the public/internal API boundary.
- Broken hello-world and kitchen-sink examples.
- Standard-library API migrations across examples and regression programs.

## 0.1.0-alpha.1 — 2026-11-02

- First packaged alpha with a complete frontend → HIR → MIR → bytecode → VM
  pipeline, core language features, standard library and Windows ZIP archive.
