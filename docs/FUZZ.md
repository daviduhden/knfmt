# Fuzzing

## American Fuzz Lop (AFL)

1. Configure and compile the binary.

   ```sh
   ./configure --pedantic
   make clean
   make AFL_USE_ASAN=1 AFL_USE_UBSAN=1 CC=afl-cc
   ```

2. Copy the binary to a safe place.

   ```sh
   cp knfmt /tmp
   ```

3. Create directories.

   ```sh
   mkdir -p fuzz/{i,o}
   ```

4. It's often of interest to exercise the latest source code changes by using
   the latest test cases as the corpus.

   ```sh
   git log --name-status tests/*.[ch] |
   sed -n 's/^A[[:space:]][[:space:]]*//p' |
   head -15 |
   xargs -I {} cp {} fuzz/i
   ```

5. Start one or many instances of afl-fuzz.

   ```sh
   afl-fuzz -i fuzz/i -o fuzz/o -M f0 /tmp/knfmt -s
   afl-fuzz -i fuzz/i -o fuzz/o -S f1 /tmp/knfmt -s
   ```
