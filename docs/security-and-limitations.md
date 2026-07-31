# Security and limitations

## Not security-audited

This driver is a functional research prototype, not a certified biometric
authentication product. It was tested with a very small number of fingers on
one sensor. Its false-accept and false-reject rates are unknown.

Never rely on it as the only access path. Keep password, recovery media, and
encrypted-disk credentials available.

## Known limitations

- Touch detection is inferred from frame activity, not a confirmed hardware
  finger-presence event.
- Thresholds are empirical and may vary with firmware, temperature, electrical
  noise, pressure, skin condition, or sensor wear.
- The matcher searches translation but not rotation, nonlinear distortion,
  pressure deformation, or liveness.
- There is no spoof or presentation-attack detection.
- The calibration table still contains one static empirical byte that should
  be populated dynamically from the sensor response.
- libfprint core is patched because its standard image pipeline assumes NBIS.
- A distribution libfprint update may break the patch or replace a local build.
- The sensor is integrated with a power button on some laptops. Rest a finger
  on it; do not mechanically click it unless power-button behavior is intended.

## Template privacy

Raw templates contain complete 70×57 fingerprint images. They are not hashes
and cannot be changed like passwords. Protect `/var/lib/fprint`, backups, logs,
crash dumps, and any diagnostic output.

The public repository deliberately excludes:

- raw or converted fingerprint images;
- enrolled GVariant templates;
- USB packet captures containing biometric data;
- hostnames, usernames, email addresses, and personal paths;
- passwords, tokens, and credentials;
- proprietary vendor binaries.

## PAM safety

Fingerprint authentication should be an alternative to a password, not a
replacement for recovery credentials. Test `fprintd-verify` before editing PAM.
Keep a root console or recovery environment available while testing.

PAM interfaces differ. Cinnamon displays its normal password field while
fingerprint verification runs in the background; there may be no dedicated
fingerprint dialog. Full-disk encryption, firmware authentication, remote SSH
keys, and non-PAM applications are outside this driver's scope.

## Responsible testing

Only capture fingerprints with informed consent. Do not publish biometric
frames. Test false acceptance with several non-enrolled fingers and repeated
placements before enabling the driver for routine authentication.
