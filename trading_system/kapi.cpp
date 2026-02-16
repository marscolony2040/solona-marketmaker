#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <map>
#include <vector>

#include <openssl/buffer.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>

#include "kapi.hpp"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <cpprest/ws_client.h>

using namespace boost::property_tree;
using namespace web;
using namespace web::websockets::client;

#define CURL_VERBOSE 0L //1L = enabled, 0L = disabled

//------------------------------------------------------------------------------

namespace Kraken {

//------------------------------------------------------------------------------
// helper function to compute SHA256:
static std::vector<unsigned char> sha256(const std::string& data)
{
   std::vector<unsigned char> digest(SHA256_DIGEST_LENGTH);

   SHA256_CTX ctx;
   SHA256_Init(&ctx);
   SHA256_Update(&ctx, data.c_str(), data.length());
   SHA256_Final(digest.data(), &ctx);

   return digest;
}

//------------------------------------------------------------------------------
// helper function to decode a base64 string to a vector of bytes:
static std::vector<unsigned char> b64_decode(const std::string& data) 
{
   BIO* b64 = BIO_new(BIO_f_base64());
   BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

   BIO* bmem = BIO_new_mem_buf((void*)data.c_str(),data.length());
   bmem = BIO_push(b64, bmem);
   
   std::vector<unsigned char> output(data.length());
   int decoded_size = BIO_read(bmem, output.data(), output.size());
   BIO_free_all(bmem);

   if (decoded_size < 0)
      throw std::runtime_error("failed while decoding base64.");
   
   return output;
}

//------------------------------------------------------------------------------
// helper function to encode a vector of bytes to a base64 string:
static std::string b64_encode(const std::vector<unsigned char>& data) 
{
   BIO* b64 = BIO_new(BIO_f_base64());
   BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

   BIO* bmem = BIO_new(BIO_s_mem());
   b64 = BIO_push(b64, bmem);
   
   BIO_write(b64, data.data(), data.size());
   BIO_flush(b64);

   BUF_MEM* bptr = NULL;
   BIO_get_mem_ptr(b64, &bptr);
   
   std::string output(bptr->data, bptr->length);
   BIO_free_all(b64);

   return output;
}

//------------------------------------------------------------------------------
// helper function to hash with HMAC algorithm:
static std::vector<unsigned char> 
hmac_sha512(const std::vector<unsigned char>& data, 
	    const std::vector<unsigned char>& key)
{   
   unsigned int len = EVP_MAX_MD_SIZE;
   std::vector<unsigned char> digest(len);

   HMAC_CTX *ctx = HMAC_CTX_new();
   if (ctx == NULL) {
       throw std::runtime_error("cannot create HMAC_CTX");
   }

   HMAC_Init_ex(ctx, key.data(), key.size(), EVP_sha512(), NULL);
   HMAC_Update(ctx, data.data(), data.size());
   HMAC_Final(ctx, digest.data(), &len);
   
   HMAC_CTX_free(ctx);
   
   return digest;
}


//------------------------------------------------------------------------------
// builds a query string from KAPI::Input (a=1&b=2&...)
static std::string build_query(const KAPI::Input& input)
{
   std::ostringstream oss;
   KAPI::Input::const_iterator it = input.begin();
   for (; it != input.end(); ++it) {
      if (it != input.begin()) oss << '&';  // delimiter
      oss << it->first <<'='<< it->second;
   }

   return oss.str();
}

//------------------------------------------------------------------------------
// helper function to create a nonce:
static std::string create_nonce()
{
   std::ostringstream oss;

   timeval tp;
   if (gettimeofday(&tp, NULL) != 0) {
      oss << "gettimeofday() failed: " << strerror(errno); 
      throw std::runtime_error(oss.str());
   }
   else {
      // format output string 
      oss << std::setfill('0') 
	  << std::setw(10) << tp.tv_sec 
	  << std::setw(6)  << tp.tv_usec;
   }
   return oss.str();
}

static std::string build_json(std::map<std::string, std::string> x){
   std::string result = "{";
   for(auto value : x){
      result += "\"" + value.first + "\":\"" + value.second + "\",";
   }
   result.pop_back();
   result += "}";
   return result;
}

//------------------------------------------------------------------------------
// constructor with all explicit parameters
KAPI::KAPI(const std::string& key, const std::string& secret, 
	   const std::string& url, const std::string& version)
   :key_(key), secret_(secret), url_(url), version_(version) 
{ 
   init(); 
}

//------------------------------------------------------------------------------
// default API base URL and API version
KAPI::KAPI(const std::string& key, const std::string& secret)
   :key_(key), secret_(secret), url_("https://api.kraken.com"), version_("0") 
{ 
   init(); 
}

//------------------------------------------------------------------------------
// constructor with empty API key and API secret
KAPI::KAPI() 
   :key_(""), secret_(""), url_("https://api.kraken.com"), version_("0") 
{ 
   init(); 
}

//------------------------------------------------------------------------------
// initializes libcurl:
void KAPI::init()
{
   curl_ = curl_easy_init();
   if (curl_) {
      curl_easy_setopt(curl_, CURLOPT_VERBOSE, CURL_VERBOSE);
      curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(curl_, CURLOPT_USERAGENT, "Kraken C++ API Client");
      curl_easy_setopt(curl_, CURLOPT_POST, 1L);
      // set callback function 
      curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, KAPI::write_cb);
   }
   else {
      throw std::runtime_error("can't create curl handle");
   }
}

//------------------------------------------------------------------------------
// destructor:
KAPI::~KAPI() 
{
   curl_easy_cleanup(curl_);
}

//------------------------------------------------------------------------------
// returns message signature generated from a URI path, a nonce 
// and postdata, message signature is created as a follows:
// 
//   hmac_sha512(path + sha256(nonce + postdata), b64decode(secret)) 
//
// and the result is converted in a base64 string: 
std::string KAPI::signature(const std::string& path, 
			    const std::string& nonce, 
			    const std::string& postdata) const
{
   // add path to data to encrypt
   std::vector<unsigned char> data(path.begin(), path.end());

   // concatenate nonce and postdata and compute SHA256
   std::vector<unsigned char> nonce_postdata = sha256(nonce + postdata);

   // concatenate path and nonce_postdata (path + sha256(nonce + postdata))
   data.insert(data.end(), nonce_postdata.begin(), nonce_postdata.end());

   // and compute HMAC
   return b64_encode( hmac_sha512(data, b64_decode(secret_)) );
}

//------------------------------------------------------------------------------
// CURL write function callback:
size_t KAPI::write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
   std::string* response = reinterpret_cast<std::string*>(userdata);
   size_t real_size = size * nmemb;

   response->append(ptr, real_size);
   return real_size;
}

//------------------------------------------------------------------------------
// deals with public API methods:
std::string KAPI::public_method(const std::string& method, 
				const KAPI::Input& input) const
{
   // build method URL
   std::string path = "/" + version_ + "/public/" + method;
   std::string method_url = url_ + path + "?" + build_query(input);
   curl_easy_setopt(curl_, CURLOPT_URL, method_url.c_str());

   // reset the http header
   curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, NULL);

   // where CURL write callback function stores the response
   std::string response;
   curl_easy_setopt(curl_, CURLOPT_WRITEDATA, static_cast<void*>(&response));

   // Set GET method
   curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);

   // perform CURL request
   CURLcode result = curl_easy_perform(curl_);
   if (result != CURLE_OK) {
      std::ostringstream oss;  
      oss << "curl_easy_perform() failed: "<< curl_easy_strerror(result);
      throw std::runtime_error(oss.str());
   }

   return response;
}

//------------------------------------------------------------------------------
// deals with private API methods:
std::string KAPI::private_method(const std::string& method, 
				 const KAPI::Input& input) const
{   
   // build method URL
   std::string path = "/" + version_ + "/private/" + method;
   std::string method_url = url_ + path;

   curl_easy_setopt(curl_, CURLOPT_URL, method_url.c_str());

   // create a nonce and and postdata 
   std::string nonce = create_nonce();
   std::string postdata = "nonce=" + nonce;

   // if 'input' is not empty generate other postdata
   if (!input.empty())
      postdata = postdata + "&" + build_query(input);
   curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, postdata.c_str());

   // add custom header
   curl_slist* chunk = NULL;

   std::string key_header =  "API-Key: "  + key_;
   std::string sign_header = "API-Sign: " + signature(path, nonce, postdata);

   chunk = curl_slist_append(chunk, key_header.c_str());
   chunk = curl_slist_append(chunk, sign_header.c_str());
   curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, chunk);
   
   // where CURL write callback function stores the response
   std::string response;
   curl_easy_setopt(curl_, CURLOPT_WRITEDATA, static_cast<void*>(&response));

   // perform CURL request
   CURLcode result = curl_easy_perform(curl_);

   // free the custom headers
   curl_slist_free_all(chunk);
  
   // check perform result
   if (result != CURLE_OK) {
      std::ostringstream oss;
      oss << "curl_easy_perform() failed: " << curl_easy_strerror(result);
      throw std::runtime_error(oss.str());
   }
   
   return response;
}

std::string KAPI::limit_buy(std::string pair, double price, double volume, std::string cl_ord_id){
   Input input;
   input.insert({"cl_ord_id", cl_ord_id});
   input.insert({"ordertype", "limit"});
   input.insert({"type", "buy"});
   input.insert({"volume", std::to_string(volume)});
   input.insert({"pair", pair});
   input.insert({"price", std::to_string(price)});

   std::string response = private_method("AddOrder", input);
   std::stringstream ss(response);
   ptree trade;
   read_json(ss, trade);

   for(ptree::const_iterator it = trade.begin(); it != trade.end(); ++it){
      if(it->first == "result"){
         for(ptree::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt){
            if(jt->first == "txid"){
               for(ptree::const_iterator kt = jt->second.begin(); kt != jt->second.end(); ++kt){
                  return kt->second.get_value<std::string>();
               }
            }
         }
      }
   }
   return "fail";
}

std::string KAPI::limit_sell(std::string pair, double price, double volume, std::string cl_ord_id){
   Input input;
   input.insert({"cl_ord_id", cl_ord_id});
   input.insert({"ordertype", "limit"});
   input.insert({"type", "sell"});
   input.insert({"volume", std::to_string(volume)});
   input.insert({"pair", pair});
   input.insert({"price", std::to_string(price)});

   std::string response = private_method("AddOrder", input);
   std::stringstream ss(response);
   ptree trade;
   read_json(ss, trade);

   for(ptree::const_iterator it = trade.begin(); it != trade.end(); ++it){
      if(it->first == "result"){
         for(ptree::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt){
            if(jt->first == "txid"){
               for(ptree::const_iterator kt = jt->second.begin(); kt != jt->second.end(); ++kt){
                  return kt->second.get_value<std::string>();
               }
            }
         }
      }
   }
   return "fail";
}

void KAPI::amend_order(std::string txid, double order_qty, double limit_price){
   Input input;
   input.insert({"txid", txid});
   input.insert({"order_qty", std::to_string(order_qty)});
   input.insert({"limit_price", std::to_string(limit_price)});

   private_method("AmendOrder", input);
}

std::string KAPI::open_orders(std::string transaction_id){
   Input input;
   std::string response = private_method("OpenOrders", input);
   std::stringstream ss(response);
   ptree df;
   read_json(ss, df);
   std::string txid_trade = "Filled";

   for(ptree::const_iterator it = df.begin(); it != df.end(); ++it){
      if(it->first == "result"){
         for(ptree::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt){
            if(jt->first == "open"){
               for(ptree::const_iterator kt = jt->second.begin(); kt != jt->second.end(); ++kt){
                  if(transaction_id == kt->first){
                     txid_trade = "NotFilled";
                  }
               }
            }
         }
      }
   }


   return txid_trade;
}

std::string KAPI::acct_balance(){
   Input input;
   std::string resp = private_method("Balance", input);
   std::stringstream ss(resp);
   ptree df;
   read_json(ss, df);
   for(ptree::const_iterator it = df.begin(); it != df.end(); ++it){
      if(it->first == "result"){
         for(ptree::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt){
            if(jt->first == "ZUSD"){
               return jt->second.get_value<std::string>();
            }
         }
      }
   }
   return "no balance";
}

std::map<std::string, double> KAPI::bidask(std::string ticker){
   Input input;
   input.insert({"pair", ticker});
   std::map<std::string, double> result;
   std::string resp = public_method("Ticker", input);
   std::stringstream ss(resp);
   ptree df;
   read_json(ss, df);
   for(ptree::const_iterator it = df.begin(); it != df.end(); ++it){
      if(it->first == "result"){
         for(ptree::const_iterator lt = it->second.begin(); lt != it->second.end(); ++lt){
            if(lt->first == ticker){
               for(ptree::const_iterator jt = lt->second.begin(); jt != lt->second.end(); ++jt){
                  if(jt->first == "a" || jt->first == "b"){
                     for(ptree::const_iterator kt = jt->second.begin(); kt != jt->second.end(); ++kt){
                        result[jt->first] = atof(kt->second.get_value<std::string>().c_str());
                        break;
                     }
                  }
               }
            }
         }
      }
   }
   return result;
}

std::string KAPI::socket_token(){
   Input input;
   std::string response = private_method("GetWebSocketsToken", input);
   std::stringstream ss(response);
   ptree df;
   read_json(ss, df);
   for(ptree::const_iterator it = df.begin(); it != df.end(); ++it){
      if(it->first == "result"){
         for(ptree::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt){
            if(jt->first == "token"){
               return jt->second.get_value<std::string>();
            }
         }
      }
   }
   return "";
}

std::string KAPI::socket_limit_buy(std::string pair, double price, double volume, std::string cl_ord_id, std::string token){
   Input input;
   input.insert({"order_type", "limit"});
   input.insert({"side", "buy"});
   input.insert({"order_qty", std::to_string(volume)});
   input.insert({"limit_price", std::to_string(price)});
   input.insert({"symbol", pair});
   input.insert({"token", token});
   input.insert({"cl_ord_id", cl_ord_id});

   std::string params = "{";
   for(auto & entry : input){
      if(entry.first == "order_qty" || entry.first == "limit_price"){
         params += "\"" + entry.first + "\":" + entry.second + ",";
      } else {
         params += "\"" + entry.first + "\":\"" + entry.second + "\",";
      }
   }
   params.pop_back();
   params += "}";

   return "{\"method\":\"add_order\",\"params\":" + params + "}";
}



//------------------------------------------------------------------------------
// helper function to initialize Kraken API library's resources:
void initialize() 
{
   CURLcode code = curl_global_init(CURL_GLOBAL_ALL);
   if (code != CURLE_OK) {
      std::ostringstream oss;
      oss << "curl_global_init() failed: " << curl_easy_strerror(code);
      throw std::runtime_error(oss.str());
   }
}



//------------------------------------------------------------------------------
// helper function to terminate Kraken API library's resources:
void terminate() 
{
   curl_global_cleanup();
}

//------------------------------------------------------------------------------

} //namespace Kraken

//------------------------------------------------------------------------------
