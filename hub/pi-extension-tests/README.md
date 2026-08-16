# Pi extension tests

Run the managed extension harness from the repository root:

```sh
node --experimental-strip-types hub/pi-extension-tests/run.mjs
```

The zero-dependency harness extracts `PiHooks.extensionSource` and checks load registration, canonical standalone-policy containment (including escapes and symlinks), hub/device failure rungs, device and TUI race outcomes, circuit breaking, and pi-permission-system step-down.
