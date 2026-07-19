# HAL Module Guidelines

## Scope
- Applies to `src/hal/` and all nested files.
- Keep the module UI-free; it uses Qt Core plus private Network/SerialPort providers.

## Architecture
- Treat `docs/design/overview/five-layer-architecture.md` and `docs/design/contracts/hal-interface-protocol.md` as the source of truth.
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
- Keep `hwtest_hal` independently linkable as a Qt Core/Network/SerialPort library target under a host/root build. The current subtree has no standalone CMake bootstrap; do not claim `cmake -S src/hal` support unless it is implemented and tested.
- Preserve the mock backend path so the module remains usable without vendor hardware.
