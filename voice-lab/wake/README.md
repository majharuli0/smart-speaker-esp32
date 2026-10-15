# Wake word lab

`wakelab.cpp` runs the speaker's own wake word matching (`../../firmware/wakecore.cpp`)
on WAV files, so it can be tuned on recordings before flashing.

```
wakelab [--sens N] take1.wav take2.wav take3.wav -- clip.wav ...
```

Each take gives its longest word; each word found in the clips prints one line:
clip, where (s), length (s), score (lower is closer), `MATCH` or `-`.
Files: 16 kHz 16-bit mono WAV (the speaker's mic test and wake word takes are
saved like that by the backend).

## Building on Windows without a compiler

Python's `ziglang` package carries a C++ compiler:

```
python -m venv venv && venv\Scripts\pip install ziglang
venv\Scripts\python -m ziglang c++ -O2 -std=c++17 -o wakelab.exe wakelab.cpp ../../firmware/wakecore.cpp
```

(Any C++17 compiler works: `g++ -O2 -o wakelab wakelab.cpp ../../firmware/wakecore.cpp`.)
