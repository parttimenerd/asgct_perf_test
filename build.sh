#!/bin/bash

cd "$(dirname "$0")" || exit 1

if [[ "$OSTYPE" == "linux-gnu"* ]]; then
  g++ src/libagent.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libagent.so -std=c++17 -shared -pthread -fPIC
elif [[ "$OSTYPE" == "darwin"* ]]; then
  c++ src/libagent.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libagent.so -std=c++17 -shared -pthread
else
  echo "Unsupported OS"
  exit 1
fi
