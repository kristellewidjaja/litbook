# Session fuzz regressions

Each file here is an input that once made `session_fuzzer` fail. It is added before the
fix (it must fail first) and kept forever. `SessionFuzzCorpusReplay` replays every file
in this folder in every build.
