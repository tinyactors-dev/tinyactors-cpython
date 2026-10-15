# Changelog: tinyactors-cpython

What changed in tinyactors-cpython in each release. The summary of a release
is written for someone upgrading; the lists below it are generated from the
commits and from the public API (docs/adr/0002-changelogs.md).

## [0.1.0] - 2026-10-15

### Summary

The first release of tinyactors for Python: SCXML statecharts run as actors
in a host, driven from Python. Charts can be written with Python as their
datamodel, or in ECMAScript (quickjs). A host can be driven from a script or
from asyncio, and the values actors exchange are plain Python objects. The
extension is a direct layer over the C library: each method mirrors one of
its calls, and the package's API map names the call behind each one.

Besides compiling charts, spawning actors and sending events, the release
exposes the rest of the host. Actors can hibernate to blobs the application
stores and later wake from them. Running actors can migrate to a new version
of their chart. A whole host can be snapshotted and restored, in this
process or another one. Charts can invoke services the application
registers, systems talk to each other through an I/O processor the
application supplies, and the host emits OpenTelemetry traces, logs and
metrics as OTLP bytes for the application to send on.

It ships as one wheel per platform for CPython 3.13 and later (the stable
ABI): macOS on Apple silicon and Intel, and Linux on x86-64 and arm64.
There is no source distribution, and the free-threaded interpreters are not
supported yet. The package is alpha: its API can change in later 0.x
releases.

