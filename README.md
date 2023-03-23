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
 bucket       %       count     min  median    mean     max     stdstd/mean
      0    74.4      133865       0       1     1.3      42     0.8     0.6
     10    16.4       29480       1       2     2.1      48     1.5     0.7
     20     6.1       10913       2       4     4.5      65     2.0     0.4
     30     2.0        3649       2       6     6.7      45     3.8     0.6
     40     1.1        1956       3       8     8.1      98     4.5     0.6
     50     0.1          90       4       5     5.6       8     0.7     0.1
     60     0.0          46       5       6     6.5       8     0.6     0.1
     70     0.0           1       8       8     7.0       8     1.0     0.1
overall     100      180000       0       1     1.8      98     1.8     1.0

signal handler till end // sending a signal and waiting till after AsyncGetCallTrace
 bucket       %       count     min  median    mean     max     stdstd/mean
      0    74.4      133865       3      10    10.6     538     3.7     0.4
     10    16.4       29480       4       9     8.6     166     3.8     0.4
     20     6.1       10913       4      12    10.9     133     4.8     0.4
     30     2.0        3649       5      11    11.7     404    10.1     0.9
     40     1.1        1956       6      13    13.5     222     9.2     0.7
     50     0.1          90       7       9     9.4      14     1.4     0.1
     60     0.0          46       8      10     9.9      14     1.0     0.1
     70     0.0           1      12      12    11.0      12     1.0     0.1
overall     100      180000       3      10    10.3     538     4.2     0.4

env  // obtaining the JNIEnv* for the thread
                     180000       0       0     0.0      12     0.1    27.0
asgct broken  // calling AsyncGetCallTrace and receiving a broken result
                      43416       0       0     0.0       3     0.1    15.3
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