# PAM example

Do not replace a complete PAM file with this snippet. Integrate it using the
distribution's PAM management tool and preserve password fallback.

The tested logical ordering was fingerprint first, then the normal Unix
password module. One example fingerprint entry is:

```pam
auth [success=2 default=ignore] pam_fprintd.so max-tries=1 timeout=30
```

The jump count depends on the surrounding PAM stack and **must not be copied
blindly**. A wrong control expression can lock out every user or bypass required
modules. On Debian/Ubuntu-family systems, prefer `pam-auth-update` or a dedicated
package profile.

Test first with `sudo -k; sudo -v` while a root/recovery console is available.
Fingerprint failure or timeout must still reach `pam_unix.so` and allow the
password.

Cinnamon's lock screen may show only its normal password field while
fingerprint verification runs in the background. On the tested setup, waiting
several seconds for calibration and then resting—not clicking—the finger on the
reader unlocked successfully.
