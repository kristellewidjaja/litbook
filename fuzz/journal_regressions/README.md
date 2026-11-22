# Journal fuzz regressions

Each file here is an input that once made `journal_fuzzer` fail. It is added before the fix
(it must fail first) and kept forever. `JournalFuzzCorpusReplay` replays every file in this
folder in every build.
