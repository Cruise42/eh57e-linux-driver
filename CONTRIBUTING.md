# Contributing

Contributions are welcome, especially reports from other confirmed
`1c7a:057e` devices, protocol corrections, synthetic tests, and safer matching
or touch-detection methods.

Before submitting anything:

1. Remove all fingerprint images and templates.
2. Remove USB captures if they contain biometric image transfers; otherwise
   sanitize them and obtain consent before publication.
3. Remove usernames, hostnames, email addresses, serial numbers, absolute home
   paths, credentials, and unrelated hardware traffic.
4. State exact USB VID/PID, firmware information if safely obtainable,
   libfprint version, distribution, and observed result.
5. Preserve password fallback while testing PAM.
6. Run the libfprint tests and build the tools with `make`.

By contributing original code to this repository, you agree to make it
available under the Zero-Clause BSD license unless the file clearly belongs to
the LGPL-covered libfprint integration patch.
