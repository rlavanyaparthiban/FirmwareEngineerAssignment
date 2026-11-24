# Evidence

| Path | What it is |
| --- | --- |
| `build/bitbake-qemuarm64-full.log` | BitBake console log of the complete `isp-image` build for `qemuarm64` (3549 tasks, all succeeded) |
| `build/bitbake-qemuarm64-final.log` | the last incremental build, which produced the image that was validated |
| `build/bitbake-raspberrypi4-64-local.log` | BitBake console log of the Raspberry Pi 4 build on the development machine: 3643 of 3644 tasks succeeded; the one failure is the download of the Raspberry Pi boot firmware, which that machine's network blocks |
| `qemu/boot.log` | serial console of the validation run, from kernel start to power-off |
| `qemu/journal-boot.log` | `journalctl -b` from the guest |
| `qemu/journal-isp-channel.log` | `journalctl -u isp-channel`: start-up, control routing, statistics every 10 s, snapshots, clean stop |
| `qemu/measurements.md` | measurement table generated from the journal, `top` samples, thread-scaling benchmark |
| `qemu/system-info.txt` | kernel, memory, device permissions, `systemd-analyze`, service status |
| `qemu/snapshot-1.png` | ISP output, automatic white balance (triggered through the control socket) |
| `qemu/snapshot-2.png` | ISP output after `Brightness 0.15` and `AwbMode tungsten` (triggered by SIGUSR1) |
| `qemu/recording-first-segment.mp4` | first one-minute H.264 segment written by the service |
| `qemu/demo.mp4` | terminal recording of a QEMU session |
| `qemu/isp-image-qemuarm64.rootfs.manifest` | packages in the root filesystem |
| `native/isp-bench-x86_64.txt` | ISP thread scaling on the x86-64 build machine |
| `native/soak-x86_64.log` | 10-minute run of the same application on the build machine: 30 fps |

The QEMU files are produced by `scripts/qemu-validate.py` and
`scripts/record-demo.py`. The builds ran on a machine whose network only
reaches a few hosts, so they used an extra, uncommitted kas overlay that points
the fetcher at the Yocto Project's HTTPS source mirror; that is why
`downloads.yoctoproject.org` appears in the logs. The overlay changes where
sources are downloaded from, not what is built.
