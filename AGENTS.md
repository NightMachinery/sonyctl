# Agent instructions for sonyctl

## Install after changes stabilize

Always install the latest build locally once changes have stabilized (feature
works, verified against the device, committed):

```sh
./install.sh          # builds Release and installs to ~/.local/bin/sonyctl
```

The point is that `sonyctl` on `PATH` is the tool actually used day to day; a
stale binary there means fixes and new flags silently don't exist for the user.
Do this after the final commit of a change, not while iterating.

Verify it took effect:

```sh
sonyctl version       # must match project(sonyctl VERSION ...) in CMakeLists.txt
```

## Other conventions

- Bump `project(sonyctl VERSION x.y.z ...)` in `CMakeLists.txt` for
  user-visible changes; `sonyctl version` reports it.
- Releases are cut by pushing a `v*` tag, which triggers
  `.github/workflows/release.yml`. Tagging publishes a public GitHub Release —
  confirm with the user before doing it.
- This repo is public: never commit personal identifiers (real device MAC
  addresses, hostnames, email, capture dumps). Use placeholder MACs such as
  `00:11:22:33:44:55` in docs and discover devices at runtime.
- Verify changes against real hardware where possible; the earbuds must be
  connected, and no other MDR client (e.g. Sound Connect on a phone) may hold
  the control channel.
- Keep `readme.org` and `docs/protocol.org` updated alongside code changes.
