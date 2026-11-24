# Validation status

Source and local build review: 24 November 2026. The checks below were completed before the initial push. No server installation or release publication is part of this review.

## Confirmed locally

- Windows x86 Release build succeeded.
- The actual DLL loads in the x86 test process. All four exports exist; API version and SDK fingerprint match the pinned SDK. Null initialization is rejected safely.
- Windows package passes PE/x86/DLL and export checks. SHA-256 checksums and a source/SDK manifest are generated. Local validation packages are explicitly marked dirty/uncommitted and must not be published.
- SDK is a Git submodule (mode `160000`) pinned to `e506d62f904567aa574c37796c072167f7513f23`. Compared against the local server SDK at `d84c2d2d259cf5a7db60ccd2966cd488ee5034ed`: only README.md differs; all SDK code is identical.
- Workflow YAML structure and packaging Python syntax were checked locally. GitHub Actions has not run for these new repositories yet.
- Public-source scan found no personal machine paths or token-shaped credentials. Local `discord.cfg` and `.env` files are ignored. This is a targeted scan, not proof that arbitrary future files contain no secrets.

- CTest: PluginExports, JsonTests and ClientTests passed (3/3). JsonTests reports 42 checks; the rate-limit regression checks reject non-finite/malformed delays and bound huge reset durations.
- Token-free Windows LiveProbe passed: HTTPS `/api/v10/gateway` and WebSocket Hello opcode 10, heartbeat interval 41,250 ms. No bot authentication or message sending occurred.

## Corrections made during review

- Track command-registration requests inside each client session, preventing request-ID collisions between different bots and clearing tracking with session destruction.
- Retain the command callback across Lua invocation; guard remaining queued results if an error event or callback destroys its client.
- Validate rate-limit header numbers before converting them to native time durations.
- Remove the unmeasured implication of complete Discord bucket-hash handling from the README. Existing per-route waits and 429 retries remain.

## Release blocker

**Linux networking is not implemented.** TransportStub.cpp returns errors for HTTPS and WebSocket connections. The Linux workflow builds and tests the portable source, but cannot establish bot functionality. Draft-release creation is blocked until the transport is implemented and validated; do not merely toggle readiness to bypass this.

## Still required

- Linux x86 build/test/package execution in GitHub Actions (no usable Linux runtime was available locally).
- Working Linux transport plus TLS/certificate, connectivity, shutdown and reconnect tests.
- Authenticated Windows bot tests: intents/permissions, incoming events, slash-command sync and replies, REST failures/429s, multiple bot clients with overlapping request IDs, self-destroying callbacks, reconnect/resume and server/script shutdown.
- Live Legacy Server Lua registration and SDK-fingerprint compatibility against the executable actually used for deployment.

The token-free probe is not a substitute for those bot/session lifecycle checks. Windows handshake cancellation can wait for native networking timeouts; do not interpret the current worker-thread model as a guarantee of instant shutdown under every network failure.
