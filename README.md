ASGCT Perf Test
===============

An agent that calls AsyncGetCallTrace in a loop on all threads and reports the time it took for the actual call.

The initial idea for the agent is to answer performance question when caching certain information in the JDK,
see the discussion to [8304725: AsyncGetCallTrace can cause SIGBUS on M1](https://github.com/openjdk/jdk/pull/13144).

But it can also be used to just run AsyncGetCallTrace continuously to check for any crashes. It is different from my [jdk profiling tester](https://github.com/parttimenerd/jdk-profiling-tester) which uses async-profiler and does not call AsyncGetCallTrace directly.
This hides some errors that async-profiler has work-arounds for.


This is related to my [asgct_bottom](https://github.com/parttimenerd/asgct_bottom) project.

It's quite simple to use (on Linux and Mac):

```sh

# build it
./build.sh

# get some benchmarks
test -e renaissance.jar || wget https://github.com/renaissance-benchmarks/renaissance/releases/download/v0.14.2/renaissance-gpl-0.14.2.jar -O renaissance.jar

# run it, e.g.
./run.sh -jar renaissance.jar -r 1 dotty

# or equally
java -agentpath=./libagent.so -jar renaissance.jar -r 1 dotty
```

The last two commands should both result in something like the following:

```
asgct alone // just calling AsyncGetCallTrace successfully (all timings in µs, with µs granularity) 
 bucket         %     count       min    median      mean       max       std  std/mean
      0      69.8     99401         0       1.0       1.0       100       1.9       1.9
     10      20.1     28558         1       1.0       1.0       527       3.5       3.5
     20       6.8      9714         1       3.0       4.0       104       3.6       0.9
     30       2.1      3051         2       4.0       7.0       656      13.3       1.9
     40       1.0      1481         3       6.0       8.0        77       6.2       0.8
     50       0.1        97         4       5.0       5.0         9       0.9       0.2
     60       0.0        48         5       6.0       6.0         7       0.6       0.1
overall       100    142350         0       1.0       2.0       656       3.3       1.7

signal handler till end // sending a signal and waiting till after AsyncGetCallTrace
 bucket         %     count       min    median      mean       max       std  std/mean
      0      69.8     99401         3      10.0      18.0     32651     256.7      14.3
     10      20.1     28558         4       6.0       7.0      4409      30.1       4.3
     20       6.8      9714         5       7.0      11.0       813      25.6       2.3
     30       2.1      3051         5       8.0      12.0       750      21.2       1.8
     40       1.0      1481         4      10.0      13.0       216      10.8       0.8
     50       0.1        97         7       9.0       9.0        14       1.2       0.1
     60       0.0        48         8      10.0       9.0        12       1.2       0.1
overall       100    142350         3      10.0      15.0     32651     215.1      14.3

env  // obtaining the JNIEnv* for the thread
                     142350         0       0.0       0.0        15       0.2       inf
asgct broken  // calling AsyncGetCallTrace and receiving a broken result
                      43867         0       0.0       0.0        15       0.1       inf
```


Using the `help` option prints all available options.


**Important on Mac**: The agent supports Mac, but might crash.

If you find any crashes, please check whether they are also appearing
with OpenJDK master and report them either to me (via issues here, Twitter or mail) or by opening a JBS issue. I'm happy to help fix them.

Developer Notes
---------------

To get proper editor support for the C++ code, use VSCode with the clangd extension and
run `bear -- ./build.sh` to generate a `compile_commands.json` file.

License
-------
MIT, Copyright 2023 SAP SE or an SAP affiliate company, Johannes Bechberger
and asgct_perf_test contributors


*This project is a tool of the [SapMachine](https://sapmachine.io) team
at [SAP SE](https://sap.com)*