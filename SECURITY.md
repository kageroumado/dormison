# Security

Report a vulnerability privately through GitHub's
[private vulnerability reporting](https://github.com/kageroumado/dormison/security/advisories/new)
rather than in a public issue. Expect an acknowledgement within a week.

In scope: a way for a Windows program running under the engine to escape the
bottle, read data the macOS user did not give it, or run code outside Wine's
own process model; and anything in `build-macos/` that would let a tampered
engine tarball pass Sevoflurane's signature check.
