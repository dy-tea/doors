# Code Style

Code is written according to the [wlroots style gudelines](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/CONTRIBUTING.md?ref_type=heads#style-reference). If you would like to automatically format your code, use my formatting tool [fovva](https://github.com/dy-tea/fovva) with the command `fovva -i -r .`.

# Usage strings

`doorsctl` and `docs/doorsctl.pod` usage strings have the following grammar:
- Full invocations start with `doorsctl ` in both places.
- Required placeholders are `<name>` in plain text and `I<name>` in POD.
- Optional placeholders are `[<name>]`in plain text and `[I<name>]` in POD.
- Repeatable placeholders are `[<name>...]` / `[I<name>...]` in POD.
- Fixed literals and choice lists are `[true|false]`, `next|last|prev`.
- Aliases are chained without spaces: `-l|--list`.
