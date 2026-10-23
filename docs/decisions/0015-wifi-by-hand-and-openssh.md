# ADR-0015: Wi-Fi typed on the board for now, and OpenSSH as the SSH server

**Status:** Accepted (the Wi-Fi part was a stop-gap; since 2026-10-23 the network is in the image, see "Update 2026-10-23")
**Date:** 2026-10-22
**Phase:** 3

## Context

After the first boot (2026-10-21) the only way into the gateway image was the serial
console. The gateway has to be on the network anyway (Phase 4 publishes with MQTT), and I
want to work on it without the serial cable. Two decisions follow: how the Pi gets the Wi-Fi
network name and password, and which SSH server goes into the image.

What the image already had (read from the root filesystem folder, 2026-10-22):

- the Wi-Fi driver and firmware (loaded at boot), `wpa_supplicant` 2.11 with `wpa_cli`,
  `wpa_passphrase` and `iw`, and the DHCP client of busybox (`udhcpc`);
- `/etc/network/interfaces` with a `wlan0` entry that uses `/etc/wpa_supplicant.conf` and DHCP,
  but is **not** marked `auto`: only `lo` and `eth0` start at boot (the Zero 2 W has no
  Ethernet, which probably explains the two `ip: SIOCGIFFLAGS: No such device` lines at boot;
  not confirmed);
- `/etc/wpa_supplicant.conf` with only a placeholder (`network={ key_mgmt=NONE }`);
- **no SSH server**.

The Wi-Fi password must never go into this repo, and a build that contains it would carry it
inside the image.

## Options considered

### Wi-Fi

#### Option A: type the network on the running board
- How it works: over the serial console I write `/etc/wpa_supplicant.conf` (the password is
  typed at the prompt of `wpa_passphrase`, which stores a derived 64-character key and not the
  password) and run `ifup wlan0`.
- Pros: no rebuild; I see every piece work; the secret is not in the repo or in any build
  output.
- Cons: a reflash erases it, so it is typed again after every flash; Wi-Fi does not start by
  itself at boot (`wlan0` is not `auto`), so a reboot needs the serial cable and `ifup wlan0`.

#### Option B: put the network in the image with a recipe in a layer of my own
- Pros: survives a reflash and can start at boot.
- Cons: the password would be in the build output and I would have to keep that file out of
  git; it forces me to create my own layer now.

#### Option C: split it: the layer makes `wlan0` start at boot, the password is typed on the board
- Pros: automatic start, no secret in the build.
- Cons: still needs my own layer first.

### SSH server

#### Option D: OpenSSH (`IMAGE_FEATURES += "ssh-server-openssh"`)
- Pros: the common one; it brings `scp` and `sftp-server`, so I can copy files from Windows to
  the Pi (the image has no Python and no other way to get a test script onto it).
- Cons: larger. Measured: the compressed image went from 79.2 MB to 80.8 MB (2008 to 2015
  packages).

#### Option E: Dropbear
- Pros: much smaller; the usual choice in small Yocto images.
- Cons: it has no `sftp` server, so copying files from a current `scp` client is awkward.

## Decision

**Option A for the Wi-Fi now, option C when my own layer exists; Option D (OpenSSH).** The
Wi-Fi is typed by hand because it keeps the password out of the repo and the build and needs
no layer yet. OpenSSH because copying files will matter and the size does not.

## Consequences

- Result of option A (2026-10-22): the scan found the network on 2462 MHz (2.4 GHz channel 11,
  signal -34 dBm); `ifup wlan0` associated, DHCP gave `192.168.1.4/24` from the router
  (lease 86,400 s), `ping` of the router answered in about 5.7 ms, `ping 1.1.1.1` in about
  15 ms (3 of 3 each), and `ping example.com` resolved a name. Then an SSH login from Windows
  worked.
- The hook that starts `wpa_supplicant` uses the old `wext` interface (`wpa-driver wext` in
  `/etc/network/interfaces`). It worked, but the kernel warned that wireless extensions "will
  stop working for Wi-Fi 7 hardware; use nl80211", and `wpa_supplicant` printed
  `ioctl[SIOCSIWENCODEEXT]: Invalid argument` twice. I do not know what those two errors
  affected, since the connection and the data work. The cure is `nl80211` in a file of my
  own layer.
- The `allow-empty-password` and `allow-root-login` features (already in `local.conf`) make
  `sshd_config` say `PermitRootLogin yes` and `PermitEmptyPasswords yes`. With a server running,
  anyone on the Wi-Fi could log in as root with no password. For now I set a password with
  `passwd` after every flash. The real fix is SSH keys and no empty password in the image.
- **A build problem.** The first build with OpenSSH produced 7 programs of 0 bytes in the
  image (`sshd`, `sshd-session`, `sshd-auth`, `ssh`, `ssh-add`, `ssh-keygen`, `ssh-keyscan`).
  The init script of `sshd` ends silently when `/usr/sbin/sshd` is not usable, so on the board
  there was no server, no host keys and no log line, and SSH answered `Connection refused`.
  Facts: the programs were complete in the source folder (for example `sshd` 1,644,920
  bytes); the copy made by `do_install` was 0 bytes for these 7 only (`ssh-agent`, `scp`,
  `sftp-server` and others were copied correctly); the copies have an earlier modification
  time (17:58:07.685) than the programs in the source folder (17:58:07.741), 30 s after the
  compile ended; and the `openssh-sshd` package was 29,913 bytes. My hypothesis, **not
  proven**, is a race: `make install`, run with 8 parallel jobs, re-links some programs while
  `install` copies them. A forced `bitbake -f -c install openssh` followed by
  `bitbake core-image-base` gave complete files (package 654,588 bytes, no zero-byte file in
  the openssh `image/` folder) and SSH worked. I cannot explain why `test -x /usr/sbin/sshd`
  was false on the board, since an empty file with the executable bit should pass it.
- Follow-up: (1) my own layer with `interfaces` (`auto wlan0`, `nl80211`) so that Wi-Fi
  starts at boot (option C); (2) SSH keys and no empty root password; (3) if the 0-byte files
  come back, a `.bbappend` for `openssh` with `PARALLEL_MAKEINST = ""`, and check after every
  build with `find <rootfs> -type f -size 0`; (4) the Pi's clock is wrong after boot (no
  battery clock): needed before any TLS use in Phase 4.
- Not verified: that the SSH host key fingerprint was compared with the one on the board (I
  only know the login worked); that the name `raspberrypi0-2w-64.local` works from Windows (the
  error type suggests it resolved, but I did not log in with it).

## Update 2026-10-23: the network is now in the image, the secret stays outside the repo

I did the Wi-Fi part of the follow-up with a variant of option B, with the key kept out of
git:

- The folder `~/yocto/secrets/` (mode 700, outside the repo) holds `wpa_supplicant.conf-sane`
  (mode 600): the three header lines of the original, `country=GR`, and a `network` block with
  the SSID and the `psk`. I made it once with a short `python3` command that asks for the
  password without showing it and derives the key the same way `wpa_passphrase` does (PBKDF2
  with HMAC-SHA1, the SSID as salt, 4096 iterations, 32 bytes). The password was never typed
  into a chat or a file.
- The recipe `wpa-supplicant` installs `/etc/wpa_supplicant.conf` from a file with exactly that
  name, `wpa_supplicant.conf-sane`. My layer has `wpa-supplicant_%.bbappend` with
  `FILESEXTRAPATHS:prepend := "/home/mariosdimopoulos/yocto/secrets:"`, the same mechanism as
  for `interfaces` in ADR-0016. The repo has only that path, never the key.
- Result: `...rootfs-20261023095901.wic.bz2`. I flashed it and powered the Pi on with no serial
  cable and typed nothing: it joined Wi-Fi by itself and SSH from Windows worked. I also read
  `config.txt` on the board and it had `dtparam=spi=on` (from `ENABLE_SPI_BUS = "1"`).
- Weak point: if the folder or the file is missing, BitBake does not fail. It installs the
  placeholder (`key_mgmt=NONE`) and the Pi would have no network. The board test was my
  evidence this time; I did not keep the output of a check of the root filesystem folder.
- The image and the build folder now contain the `psk`, which is as good as the Wi-Fi password.
  The `.wic.bz2` files must not be shared or uploaded; the SD card can be read by anyone who
  holds it. A rule in `.gitignore` keeps a `secrets/` folder out of git. The key has to go
  before the image is a final deliverable.
- Not done: the password is still not in a form that other people can rebuild from the repo;
  they need to create their own `secrets/` folder (steps in `linux-gateway/README.md`).

## Interview angle

"I kept the Wi-Fi password out of the repo and the image by typing it on the board for the
first bring-up, and put OpenSSH in the image because I needed `scp`. The first build shipped
seven empty binaries; I traced it from the init script that exited silently, to the install
step, to the timestamps, forced the install again and wrote down that the cause is a likely
parallel-make race that I have not proven."
