# Fuzz regressions

Each file here is an input that once made `book_fuzzer` fail. It is added before the
fix (it must fail first) and kept forever; `FuzzCorpusReplay` replays every file in
this folder in every build.
