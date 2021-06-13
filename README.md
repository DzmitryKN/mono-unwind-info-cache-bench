Requires C++11-capable GCC.

To run benchmark, run: `make` then wait til completion of both flavours tested (~less than 2 minutes) and compare numbers (less is better) inside `Test complete in ???? microseconds`.

To run endurance testing for some of the flavour, run: `make endurance_original` or `make endurance_optimized` - note that endurance tests designed to run forever unless you will press Ctrl+C.
