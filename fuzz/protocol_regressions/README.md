# Protocol fuzz regressions

Each file here is an input that once made `protocol_fuzzer` fail. It is added before the
fix (it must fail first) and kept forever. `ProtocolFuzzCorpusReplay` replays every file
in this folder in every build.
