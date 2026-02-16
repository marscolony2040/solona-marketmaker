#include <iostream>
#include <string>
#include <chrono>
#include <vector>
#include <map>
#include <time.h>
#include <thread>
#include "kapi.hpp"
#include <cpprest/ws_client.h>
#include <sstream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <thread>    // Required for std::this_thread::sleep_for
#include <chrono>

using namespace web;
using namespace web::websockets::client;
using namespace boost::property_tree;

// Copy-Paste Kraken Key and Secret
const std::string& key = "";
const std::string& secret = "";


int main()
{
    // Load trader class
    Kraken::KAPI kapi(key, secret);

    // This will hold the highest bid/ lowest ask
    std::map<std::string, double> bidask;

    // Sets bid/ask margin and price increments
    double margin = 0.5;
    double cc = 0.1;


    std::cout << "Solona Market Maker" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "Go for it" << std::endl;
    while(true){

        // Import Highest Bid and Lowest Ask
        bidask = kapi.bidask("SOLUSD");

        // Highest bid
        double bid = bidask["b"];

        // Place buy limit order and edit until it is filled
        std::string tid = kapi.limit_buy("SOLUSD", (bid - margin) + cc, 0.02, "ClassOf2013");
        while(true){
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Checks if limit buy order is filled or not
            std::string is_it_filled = kapi.open_orders(tid);
            std::cout << is_it_filled << " " << tid << std::endl;
            if(is_it_filled == "Filled"){
                std::cout << "Buy order has been filled" << std::endl;
                break;
            } else {
                // Edits the limit buy order in order to fill
                bidask = kapi.bidask("SOLUSD");
                double ba = bidask["b"];
                cc += 0.1;
                kapi.amend_order(tid, 0.02, (ba - margin) + cc);
                std::cout << "Editing buy order: " << (ba - margin) + cc << " " << cc << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Exit the trade with a limit order, same way as entry
        cc = 0.1;
        bidask = kapi.bidask("SOLUSD");
        double ask = bidask["a"];

        // Place limit sell order
        tid = kapi.limit_sell("SOLUSD", (ask + margin) - cc, 0.02, "ClassOf2013");
        while(true){
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Check to see if limit sell order is filled
            std::string is_it_filled = kapi.open_orders(tid);
            std::cout << is_it_filled << " " << tid << std::endl;
            
            if(is_it_filled == "Filled"){
                std::cout << "Sell order has been filled" << std::endl;
                break;
            } else {
                // Edits sell order price in order to fill
                bidask = kapi.bidask("SOLUSD");
                double aa = bidask["a"];
                cc += 0.1;
                kapi.amend_order(tid, 0.02, (aa + margin) - cc);
                std::cout << "Editing sell order: " << (aa + margin) - cc << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        // Displays balance after trades have completed
        std::cout << "Balance: " << kapi.acct_balance() << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    

    return 0;
}


