# rectest: video_writer.cpp on the host

Runs the real `video_writer.cpp` (no ESP32) against a simulated camera and SD card,
then checks the `.mov` with ffprobe. Built to compare recorder designs on identical
input: 40 ms +/- 3 ms frames, two 120 ms gaps, SD write stalls, viewfinder threads
competing for frames the way the web page and the OS viewfinder do.

    mkdir frames   # f01.jpg .. f39.jpg, any 800x600 JPEGs
    c++ -std=c++17 -O1 -DNEWAPI -I tools/rectest/shim -I . \
        -include tools/rectest/shim/Arduino.h video_writer.cpp tools/rectest/rectest_main.cpp -o rec -lpthread
    ./rec out.mov <secs> <fps ceiling> <stall every N writes> <stall ms> <viewers> [quarter turns clockwise]
    python3 tools/rectest/chk.py out.mov

Drop `-DNEWAPI` to build against a recorder that has no `camFrameAcquire`.
