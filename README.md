# Solana Market Maker
I forked this repository from voidloop and I built a Solana market making system which trades on Kraken. My system fetches the highest bid and quotes an order several depths away and edits the order until it has been filled. When the order has been filled I execute an algorithmic limit sell order and edit the price until it has been filled. This completes the trading cycle and recursively buys and sells in order to capture the bid/ask spread. My modifications are located in
the folder trading_system. This is built fully with C++

I also have a video on my YouTube channel MoQuant where I showcase how to build this system from scratch and the video can be found at this link
[MarketMakingVideo](https://www.youtube.com/watch?v=cHwRqlpE0J8)

krakenapi
=========

A C++ library for interfacing with the Kraken REST API (kraken.com).

Other programs
==============

krt
---

Source file of this program is krt.cpp.

### What is krt?

krt is a program to download Recent Trades from Kraken market data through API.  

### How trades are displayed? 
 
Recent trades are printed out to standard output in CSV format. The order of fields is "Time", "Order", "Price" and "Volume".

### Command line arguments

usage: krt \<pair\> \[interval\] \[since\]

krt can get the following command line arguments:

  \<pair\>   
  Asset pair to get trade data for.

  \[since\]  
  (Optional) the program returns trade data since given id. By default [since] is equal 
  to "0" to indicate the oldest possible trade data.

  \[interval\] 
  (Optional) how many seconds the program has to download new trade data. 
  By default the program doesn't use this parameter and it exits immidiatly after 
  download trade data. If [interval] is equal to 0 the program will not 
  use this parameter.
