# Snapshot fuzz regressions

Each file here is an input that once made `snapshot_fuzzer` fail. It is added before the fix
(it must fail first) and kept forever. `SnapshotFuzzCorpusReplay` replays every file in this
folder in every build.
