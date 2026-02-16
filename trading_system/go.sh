#!/bin/bash
echo "Compiling System"
rm -rf a.out
g++ trader.cpp api/kapi.cpp -std=c++17 -lcurl -lcrypto -lcpprest -lssl
echo "Compiled"
exit 0
