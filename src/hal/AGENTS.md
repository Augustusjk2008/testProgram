# HAL Module Guidelines

## Scope
- Applies to `src/hal/` and all nested files.
- Keep the module UI-free; it is a core Qt library only.

## Architecture
- Treat `docs/design/five-layer-architecture.md` and `docs/design/hal-interface-protocol.md` as the source of truth.
- Keep public HAL headers under `src/hal/include/hal/` stable and minimal.
- Keep internal implementation in `src/hal/src/`.
- Keep the mock backend working first; it is the default development path.

## Style
- Use Qt 5.15 / C++17 conventions and `hwtest::hal` namespaces.
- Add short comments where intent or edge cases matter; avoid comment noise.
- Prefer small focused helpers over large catch-all classes.

## Change Control
- Update the design docs when the HAL interface or config shape changes.
- Extend structs at the tail when possible and avoid breaking enum semantics.
- Keep factory, service, device, and backend seams explicit so future adapters can be swapped in cleanly.

## Build
- The HAL module must continue to build as a standalone Qt Core library.
- Preserve the mock backend path so the module remains usable without vendor hardware.
