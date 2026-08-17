# KernelSU Next for Waydroid x86_64

KernelSU Next adapted to an x86_64 Waydroid container sharing the host kernel.
The project builds an external `kernelsu.ko` module and an Android `ksud`; it
does not rebuild the full kernel.

The workflow is exposed through one command:

```sh
./waydroid-kernelsu package
sudo ./waydroid-kernelsu install
sudo waydroid-kernelsu configure
```

See [WAYDROID.md](WAYDROID.md) for architecture, requirements, safe unload,
internal hooks, and runtime verification.
