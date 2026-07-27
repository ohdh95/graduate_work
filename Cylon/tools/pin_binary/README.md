## Pin executables and libraries on NUMA node 1

* How to build
```sh
# compile
./build.sh

# for printing debug message
./build.sh debug
```

* How to use: add `LD_PRELOAD=/path/to/pin_binary.so`
```sh
LD_PRELOAD=/path/to/pin_binary.so ./run.sh
```