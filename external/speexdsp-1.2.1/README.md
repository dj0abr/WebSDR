# SpeexDSP 1.2.1 bundle

Copied unchanged (library, public headers, license and checksum manifest) from
the QO100_TRX2 SpeexDSP 1.2.1 bundle for the same voice denoiser.
The root project's `NoiseReduction.cpp` adapts the QO100 wrapper to 8 kHz float
streams and the WebSDR's per-listener processing.

This directory contains the statically linked Linux x86-64 library, matching
public headers, BSD license (`LICENSE`) and `manifest-linux-x86_64.json`. The
manifest records the upstream release URL, source hash, build flags and hashes
of the bundled files. No source download occurs during the ordinary build.
The library was built with generic x86-64 instructions, floating point and the
built-in smallft FFT, without a separate FFTW dependency inside the archive.

Copy the entire directory when moving the project to another Linux x86-64 host.
For other architectures the Makefile uses the system SpeexDSP development package
(`libspeexdsp-dev`), or a matching archive can be provided under
`lib/linux-<architecture>/libspeexdsp.a` together with matching headers.
Retain `LICENSE` with source/binary distributions.
