/// \file conn_http.cpp
/// Contains the main code for the HTTP Connector

#include <iostream>
#include <queue>
#include <set>

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <getopt.h>

#include <mist/socket.h>
#include <mist/http_parser.h>
#include <mist/config.h>
#include <mist/stream.h>
#include <mist/timing.h>
#include <mist/auth.h>

#include "tinythread.h"
#include "embed.js.h"


//sharva_modn_final
#include <fstream>
#include <sys/socket.h>
#include <sys/epoll.h>
#define __NR_map_back 318
#define __NR_vnet_yank 319
#define __NR_vnet_yankputdata 320
#define __NR_vnet_yankdata 321
#define __NR_vsock_yank 317


/// Holds everything unique to HTTP Connectors.
namespace Connector_HTTP {

/// Class for keeping track of connections to connectors.
class ConnConn{
public:
	Socket::Connection * conn; ///< The socket of this connection
	unsigned int lastUse; ///< Seconds since last use of this connection.
	tthread::mutex inUse; ///< Mutex for this connection.
	/// Constructor that sets the socket and lastUse to 0.
	ConnConn(){
		conn = 0;
		lastUse = 0;
	}
	;
	/// Constructor that sets lastUse to 0, but socket to s.
	ConnConn(Socket::Connection * s){
		conn = s;
		lastUse = 0;
	}
	;
	/// Destructor that deletes the socket if non-null.
	~ConnConn(){
		if (conn){
			conn->close();
			delete conn;
		}
		conn = 0;
	}
	;
};

std::map<std::string, ConnConn *> connectorConnections; ///< Connections to connectors
tthread::mutex connMutex; ///< Mutex for adding/removing connector connections.
tthread::mutex timeoutMutex; ///< Mutex for timeout thread.
tthread::thread * timeouter = 0; ///< Thread that times out connections to connectors.

///\brief Function run as a thread to timeout requests on the proxy.
///\param n A NULL-pointer
void proxyTimeoutThread(void * n){
	n = 0; //prevent unused variable warning
	tthread::lock_guard<tthread::mutex> guard(timeoutMutex);
	while (true){
		{
			tthread::lock_guard<tthread::mutex> guard(connMutex);
			if (connectorConnections.empty()){
				return;
			}
			std::map<std::string, ConnConn*>::iterator it;
			for (it = connectorConnections.begin(); it != connectorConnections.end(); it++){
				ConnConn* ccPointer = it->second;
				if ( !ccPointer->conn->connected() || ccPointer->lastUse++ > 15){
					if (ccPointer->inUse.try_lock()){
						connectorConnections.erase(it);
						ccPointer->inUse.unlock();
						delete ccPointer;
						it = connectorConnections.begin(); //get a valid iterator
						if (it == connectorConnections.end()){
							return;
						}
					}
				}
			}
			connMutex.unlock();
		}
		usleep(1000000); //sleep 1 second and re-check
	}
}

///\brief Handles requests without associated handler.
///
///Displays a friendly error message.
///\param H The request to be handled.
///\param conn The connection to the client that issued the request.
///\return A timestamp indicating when the request was parsed.
long long int proxyHandleUnsupported(HTTP::Parser & H, Socket::Connection * conn){
	H.Clean();
	H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
	H.SetBody(
			"<!DOCTYPE html><html><head><title>Unsupported Media Type</title></head><body><h1>Unsupported Media Type</h1>The server isn't quite sure what you wanted to receive from it.</body></html>");
	long long int ret = Util::getMS();
	conn->SendNow(H.BuildResponse("415", "Unsupported Media Type"));
	return ret;
}

///\brief Handles requests that have timed out.
///
///Displays a friendly error message.
///\param H The request that was being handled upon timeout.
///\param conn The connection to the client that issued the request.
///\return A timestamp indicating when the request was parsed.
long long int proxyHandleTimeout(HTTP::Parser & H, Socket::Connection * conn){
	H.Clean();
	H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
	H.SetBody(
			"<!DOCTYPE html><html><head><title>Gateway timeout</title></head><body><h1>Gateway timeout</h1>Though the server understood your request and attempted to handle it, somehow handling it took longer than it should. Your request has been cancelled - please try again later.</body></html>");
	long long int ret = Util::getMS();
	conn->SendNow(H.BuildResponse("504", "Gateway Timeout"));
	return ret;
}

///\brief Handles requests within the proxy.
///
///Currently supported urls:
/// - /crossdomain.xml
/// - /clientaccesspolicy.xml
/// - *.ico (for favicon)
/// - /info_[streamname].js (stream info)
/// - /embed_[streamname].js (embed info)
///
///Unsupported urls default to proxyHandleUnsupported( ).
///\param H The request to be handled.
///\param conn The connection to the client that issued the request.
///\return A timestamp indicating when the request was parsed.
long long int proxyHandleInternal(HTTP::Parser & H, Socket::Connection * conn){

	std::string url = H.getUrl();

	if (url == "/crossdomain.xml"){
		H.Clean();
		H.SetHeader("Content-Type", "text/xml");
		H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
		H.SetBody(
				"<?xml version=\"1.0\"?><!DOCTYPE cross-domain-policy SYSTEM \"http://www.adobe.com/xml/dtds/cross-domain-policy.dtd\"><cross-domain-policy><allow-access-from domain=\"*\" /><site-control permitted-cross-domain-policies=\"all\"/></cross-domain-policy>");
		long long int ret = Util::getMS();
		conn->SendNow(H.BuildResponse("200", "OK"));
		return ret;
	} //crossdomain.xml

	if (url == "/clientaccesspolicy.xml"){
		H.Clean();
		H.SetHeader("Content-Type", "text/xml");
		H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
		H.SetBody(
				"<?xml version=\"1.0\" encoding=\"utf-8\"?><access-policy><cross-domain-access><policy><allow-from http-methods=\"*\" http-request-headers=\"*\"><domain uri=\"*\"/></allow-from><grant-to><resource path=\"/\" include-subpaths=\"true\"/></grant-to></policy></cross-domain-access></access-policy>");
		long long int ret = Util::getMS();
		conn->SendNow(H.BuildResponse("200", "OK"));
		return ret;
	} //clientaccesspolicy.xml

	// send logo icon
	if (url.length() > 4 && url.substr(url.length() - 4, 4) == ".ico"){
		H.Clean();
#include "icon.h"
		H.SetHeader("Content-Type", "image/x-icon");
		H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
		H.SetHeader("Content-Length", icon_len);
		H.SetBody("");
		long long int ret = Util::getMS();
		conn->SendNow(H.BuildResponse("200", "OK"));
		conn->SendNow((const char*)icon_data, icon_len);
		return ret;
	}

	if ((url.length() > 9 && url.substr(0, 6) == "/info_" && url.substr(url.length() - 3, 3) == ".js")
			|| (url.length() > 10 && url.substr(0, 7) == "/embed_" && url.substr(url.length() - 3, 3) == ".js")){
		std::string streamname;
		if (url.substr(0, 6) == "/info_"){
			streamname = url.substr(6, url.length() - 9);
		}else{
			streamname = url.substr(7, url.length() - 10);
		}
		Util::Stream::sanitizeName(streamname);
		JSON::Value ServConf = JSON::fromFile("/tmp/mist/streamlist");
		std::string response;
		std::string host = H.GetHeader("Host");
		if (host.find(':')){
			host.resize(host.find(':'));
		}
		H.Clean();
		H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
		H.SetHeader("Content-Type", "application/javascript");
		response = "// Generating info code for stream " + streamname + "\n\nif (!mistvideo){var mistvideo = {};}\n";
		JSON::Value json_resp;
		if (ServConf["streams"].isMember(streamname) && ServConf["config"]["protocols"].size() > 0){
			json_resp["width"] = ServConf["streams"][streamname]["meta"]["video"]["width"].asInt();
			json_resp["height"] = ServConf["streams"][streamname]["meta"]["video"]["height"].asInt();
			if (json_resp["width"].asInt() < 1 || json_resp["height"].asInt() < 1){
				json_resp["width"] = 640ll;
				json_resp["height"] = 480ll;
			}
			if (ServConf["streams"][streamname]["meta"].isMember("vod")){
				json_resp["type"] = "vod";
			}
			if (ServConf["streams"][streamname]["meta"].isMember("live")){
				json_resp["type"] = "live";
			}
			//find out which connectors are enabled
			std::set<std::string> conns;
			for (JSON::ArrIter it = ServConf["config"]["protocols"].ArrBegin(); it != ServConf["config"]["protocols"].ArrEnd(); it++){
				conns.insert(( *it)["connector"].asString());
			}
			//first, see if we have RTMP working and output all the RTMP.
			for (JSON::ArrIter it = ServConf["config"]["protocols"].ArrBegin(); it != ServConf["config"]["protocols"].ArrEnd(); it++){
				if (( *it)["connector"].asString() == "RTMP"){
					if (( *it)["port"].asInt() == 0){
						( *it)["port"] = 1935ll;
					}
					JSON::Value tmp;
					tmp["type"] = "rtmp";
					tmp["url"] = "rtmp://" + host + ":" + ( *it)["port"].asString() + "/play/" + streamname;
					json_resp["source"].append(tmp);
				}
			}
			/// \todo Add raw MPEG2 TS support here?
			//then, see if we have HTTP working and output all the HTTP.
			for (JSON::ArrIter it = ServConf["config"]["protocols"].ArrBegin(); it != ServConf["config"]["protocols"].ArrEnd(); it++){
				if (( *it)["connector"].asString() == "HTTP"){
					if (( *it)["port"].asInt() == 0){
						( *it)["port"] = 8080ll;
					}
					// check for dynamic
					if (conns.count("HTTPDynamic")){
						JSON::Value tmp;
						tmp["type"] = "f4v";
						tmp["url"] = "http://" + host + ":" + ( *it)["port"].asString() + "/dynamic/" + streamname + "/manifest.f4m";
						tmp["relurl"] = "/dynamic/" + streamname + "/manifest.f4m";
						json_resp["source"].append(tmp);
					}
					// check for smooth
					if (conns.count("HTTPSmooth")){
						JSON::Value tmp;
						tmp["type"] = "ism";
						tmp["url"] = "http://" + host + ":" + ( *it)["port"].asString() + "/smooth/" + streamname + ".ism/Manifest";
						tmp["relurl"] = "/smooth/" + streamname + ".ism/Manifest";
						json_resp["source"].append(tmp);
					}
					// check for HLS
					if (conns.count("HTTPLive")){
						JSON::Value tmp;
						tmp["type"] = "hls";
						tmp["url"] = "http://" + host + ":" + ( *it)["port"].asString() + "/hls/" + streamname + "/index.m3u8";
						tmp["relurl"] = "/hls/" + streamname + "/index.m3u8";
						json_resp["source"].append(tmp);
					}
					// check for progressive
					if (conns.count("HTTPProgressive")){
						JSON::Value tmp;
						tmp["type"] = "flv";
						tmp["url"] = "http://" + host + ":" + ( *it)["port"].asString() + "/" + streamname + ".flv";
						tmp["relurl"] = "/" + streamname + ".flv";
						json_resp["source"].append(tmp);
					}
				}
			}
		}else{
			json_resp["error"] = "The specified stream is not available on this server.";
		}
		response += "mistvideo['" + streamname + "'] = " + json_resp.toString() + ";\n";
		if (url.substr(0, 6) != "/info_" && !json_resp.isMember("error")){
			response.append("\n(");
			response.append((char*)embed_js, (size_t)embed_js_len - 2); //remove trailing ";\n" from xxd conversion
			response.append("(\"" + streamname + "\"));\n");
		}
		H.SetBody(response);
		long long int ret = Util::getMS();
		conn->SendNow(H.BuildResponse("200", "OK"));
		return ret;
	} //embed code generator

	return proxyHandleUnsupported(H, conn); //anything else doesn't get handled
}




///\brief Handles requests by dispatching them to the corresponding connector.
///\param H The request to be handled.
///\param conn The connection to the client that issued the request.
///\param connector The type of connector to be invoked.
///\return A timestamp indicating when the request was parsed.
long long int proxyHandleThroughConnector(HTTP::Parser & H, Socket::Connection * conn, std::string & connector){


	std::ofstream myfile  ("/home/sharvanath/mistserver/mistserver_back/debug/http.txt",std::ios::app);

	//create a unique ID based on a hash of the user agent and host, followed by the stream name and connector
	std::string uid = Secure::md5(H.GetHeader("User-Agent") + conn->getHost()) + "_" + H.GetVar("stream") + "_" + connector;
	H.SetHeader("X-Stream", H.GetVar("stream"));
	H.SetHeader("X-UID", uid); //add the UID to the headers before copying
	H.SetHeader("X-Origin", conn->getHost()); //add the UID to the headers before copying
	std::string request = H.BuildRequest(); //copy the request for later forwarding to the connector
	std::string orig_url = H.getUrl();
	H.Clean();

	//check if a connection exists, and if not create one
	connMutex.lock();
	if ( !connectorConnections.count(uid) || !connectorConnections[uid]->conn->connected()){
		if (connectorConnections.count(uid)){
			delete connectorConnections[uid];
			connectorConnections.erase(uid);
		}
		connectorConnections[uid] = new ConnConn(new Socket::Connection("/tmp/mist/http_" + connector));
		connectorConnections[uid]->conn->setBlocking(false); //do not block on spool() with no data

		syscall(__NR_map_back, conn->sock, connectorConnections[uid]->conn->sock);
		myfile << "Created new connection " << std::endl;
		myfile.close();
#if DEBUG >= 4
		std::cout << "Created new connection " << uid << std::endl;
#endif
	}else{
#if DEBUG >= 5
		std::cout << "Re-using connection " << uid << std::endl;
#endif
	}
	//start a new timeout thread, if neccesary
	if (timeoutMutex.try_lock()){
		if (timeouter){
			timeouter->join();
			delete timeouter;
		}
		timeouter = new tthread::thread(Connector_HTTP::proxyTimeoutThread, 0);
		timeoutMutex.unlock();
	}

	//lock the mutex for this connection, and handle the request
	ConnConn * myCConn = connectorConnections[uid];
	myCConn->inUse.lock();
	connMutex.unlock();
	//if the server connection is dead, handle as timeout.
	if ( !myCConn->conn->connected()){
		myCConn->conn->close();
		return proxyHandleTimeout(H, conn);
	}
	//forward the original request
	myCConn->conn->SendNow(request);
	myCConn->lastUse = 0;
	unsigned int timeout = 0;
	unsigned int retries = 0;


	void *ptr1 = mmap (NULL, MAPPED, PROT_WRITE | PROT_READ, MAP_SHARED, conn->sock, 0);
	conn->stats = ( struct tcp_sock_stats * ) ptr1;

	//wait for a response
	while (myCConn->conn->connected() && conn->connected()){
		conn->spool();
		if (myCConn->conn->Received().size() || myCConn->conn->spool()){
			//make sure we end in a \n
			if ( *(myCConn->conn->Received().get().rbegin()) != '\n'){
				std::string tmp = myCConn->conn->Received().get();
				myCConn->conn->Received().get().clear();
				if (myCConn->conn->Received().size()){
					myCConn->conn->Received().get().insert(0, tmp);
				}else{
					myCConn->conn->Received().append(tmp);
				}
			}
			//check if the whole response was received
			if (H.Read(myCConn->conn->Received().get())){
				//208 means the fragment is too new, retry in 3s
				if (H.url == "208"){
					retries++;
					if (retries >= 5){
						std::cout << "[5 retry-laters, cancelled]" << std::endl;
						myCConn->conn->close();
						myCConn->inUse.unlock();
						return proxyHandleTimeout(H, conn);
					}
					myCConn->lastUse = 0;
					timeout = 0;
					Util::sleep(3000);
					myCConn->conn->SendNow(request);
					H.Clean();
					continue;
				}
				break; //continue down below this while loop
			}
		}else{
			//keep trying unless the timeout triggers
			if (timeout++ > 4000){
				//if (0){
				std::cout << "[20s timeout triggered]" << std::endl;
				myCConn->conn->close();
				myCConn->inUse.unlock();
				return proxyHandleTimeout(H, conn);
			}else{
				Util::sleep(5);
			}
		}
	}
	if ( !myCConn->conn->connected() || !conn->connected()){
		//failure, disconnect and sent error to user
		myCConn->conn->close();
		myCConn->inUse.unlock();
		return proxyHandleTimeout(H, conn);
	}else{
		long long int ret = Util::getMS();
		//success, check type of response
		if (H.GetHeader("Content-Length") != ""){
			//known length - simply re-send the request with added headers and continue

			/*H.SetHeader("X-UID", uid);
			H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
			conn->SendNow(H.BuildResponse("200", "OK"));
			std::cerr<<"sent\n";
			myCConn->inUse.unlock();
			 */



			//sharva_modn
			std::cerr<<"mapped\n";
			syscall(__NR_map_back, conn->sock, myCConn->conn->sock);


			/////sharva_modn
			H.SetHeader("X-UID", uid);
			H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
			//std::ofstream myfile  ("/home/sharvanath/mistserver/mistserver_back/debug/test_map.txt",std::ios::app);
			//myfile << "the stats here are : cwnd="<<conn->stats->snd_cwnd<<" rtt="<<conn->stats->srtt<<"\n";
			//std::string resp = H.BuildResponse("200", "OK");
			//sharva_mod
			//

			//std::cerr<<"\n before send: "<<myCConn->conn->sock<<"\n";

			std::string body = H.body;

			H.SetBody("");
			//H.body = "";
			//sharva_modn_final
			//conn->SendChunkNow(resp);
			//H.SetHeader("Content-Length","0");
			H.SetHeader("Transfer-Encoding", "chunked");
			//H.Chunkify(body);
			conn->SendNow(H.BuildResponse("200", "OK"));

			int send_length = body.length(),curr_length=0,pos = 0;
			//char buff[1000] = "aaaaaaaaa";

			int init_size[50000],sent_size[50000],sent_size1[50000],sent_idx=0;

			std::ofstream myfile  ("/home/sharvanath/mistserver/mistserver_back/debug/http.txt",std::ios::app);
			
			int printed = 0;
			
			//just a hack, fix this 1000
			while(send_length>0 || conn->stats->write_seq != conn->stats->snd_una) {

				if(send_length==0 && printed==0)
				{
					std::cerr<<"pushed\n";
					printed = 1;
				}
					
				//std::cerr<<"here1\n"<<conn->stats->write_seq<<" "<<conn->stats->snd_una<<"\n";
				
				if(send_length>0){
					curr_length = (send_length<1000?send_length:1000);
					std::string str  = body.substr(pos,curr_length);
					pos += curr_length;
					send_length -= curr_length;
					//std::cerr<<"here2\n";
					init_size[sent_idx] = str.size();
					//std::cerr<<"here3\n";
					H.Chunkify(str);
					sent_size[sent_idx] = str.size();
					//std::cerr<<"here4\n";

					if(sent_idx>0){
						init_size[sent_idx] += init_size[sent_idx-1];
						sent_size1[sent_idx] = sent_size[sent_idx] + sent_size1[sent_idx-1];
					}

					sent_idx++;


					conn->SendNow(str);
				}
				int is_active  = syscall(__NR_vnet_yank, myCConn->conn->sock, 1, -1);


				//if(conn->stats->srtt!=0)
				//std::cerr << "bandwidth in http ="<<(conn->stats->snd_cwnd*1000/conn->stats->srtt);

				if(is_active==0){
					//start the yank procedure and block here..........sleep using events,

					//yank data from succeeding modules and stall this connection.
					myfile<<"yank request locking send\n";
					//std::cerr<<"yank request locking send\n";


					int amount  = syscall(__NR_vnet_yank, conn->sock, 2, -1);	//ask to check the maximum amount yankable

					myfile<<"amount ="<<amount<<"\n";

					//std::cerr<<"amount ="<<amount<<"\n";
					char * buff = new char[amount];

					amount = syscall(__NR_vsock_yank, conn->sock, buff, amount, amount);

					myfile<<"amount ="<<amount<<"\n";
					//std::cerr<<"amount ="<<amount<<"\n";

					//this should lock the send

					//int this_chunk  = (body.length() - amount - send_length) > 0? body.length() - amount - send_length : 0;

					int i = 0, rev_send = 0;
					//going reverse to see what all are yankable

					for(i=sent_idx-1;i>=0;i--)
					{
						rev_send += sent_size[i];
						if(rev_send>amount)
							break;
					}


					//handle i==0

					myfile<<"sent_idx = "<<sent_idx<<"i ="<<i<<"\n";
					//std::cerr<<"sent_idx = "<<sent_idx<<"i ="<<i<<"\n";


					myfile<<"I need = "<< sent_size1[sent_idx-1] - sent_size1[i]<<"\n";
					//std::cerr<<"I need = "<< sent_size1[sent_idx-1] - sent_size1[i]<<"\n";


					syscall(__NR_vnet_yank, conn->sock, 4, 0);

					if(amount > (sent_size1[sent_idx-1] - sent_size1[i]))
						conn->SendNow(buff,amount - (sent_size1[sent_idx-1] - sent_size1[i]));


					//init_size[sent_idx-1]
					//finally, conn->SendNow(buff+sent_size[i-1],amount-sent_size[i-1]);
					myfile<<"from the socket it's yanked = "<<amount<<" Sent back "<<amount - (sent_size1[sent_idx-1] - sent_size1[i]) <<"\n";
					//std::cerr<<"from the socket it's yanked = "<<amount<<" Sent back "<<amount - (sent_size1[sent_idx-1] - sent_size1[i]) <<"\n";

					//yank from the buffer    ,          init_size[sent_idx-1]+send_length
					int yank_data  = syscall(__NR_vnet_yank, myCConn->conn->sock, 1, send_length + init_size[sent_idx-1] - init_size[i]);

					myfile<<"yank data is set to "<<yank_data-1<<"\n";
					//std::cerr<<"yank data is set to "<<yank_data-1<<"\n";

					myfile.close();

					do 
					{
						std::ofstream myfile1  ("/home/sharvanath/mistserver/mistserver_back/debug/http.txt",std::ios::app);
						yank_data  = syscall(__NR_vnet_yank, myCConn->conn->sock, 3, 0);
						myfile1<<"asked for yank "<<yank_data<<"\n";
						//std::cerr<<"asked for yank "<<yank_data<<"\n";
						myfile1.close();
					} 
					while(yank_data<=0);


					std::ofstream myfile1  ("/home/sharvanath/mistserver/mistserver_back/debug/http.txt",std::ios::app);

					myfile1<<"asked for yank "<<yank_data<<"\n";
					//std::cerr<<"asked for yank "<<yank_data<<"\n";

					//if(yank_data<send_length)
					//do what?

					if(i!=sent_idx-1)
					{
						body = body.substr(init_size[i+1], send_length + init_size[sent_idx-1] - init_size[i] - yank_data);
						pos = 0;
					}
					send_length =  send_length + init_size[sent_idx-1] - init_size[i+1] - yank_data;


					myfile1<<"pending to chunkify is "<<send_length<<"\n";
					//std::cerr<<"pending to chunkify is "<<send_length<<"\n";

					myfile1.close();
					//while(1);


					//while(1);
					//yanked

					//send_length -= yank_data;
					//char * buff = new char[yank_data];
					//buff[0] = 'a';buff[0] = 'b';buff[0] = 'c';
					//int sent_data_size = syscall(__NR_vnet_yankputdata, myCConn->conn->sock, buff, yank_data);

					//std::cerr<<"yank sent "<<sent_data_size<<"\n";

					//while(1);


					syscall(__NR_vnet_yank, myCConn->conn->sock, 4, 0);
					/*do 
					{
						is_active  = syscall(__NR_vnet_yank, myCConn->conn->sock, 1, -1);
					} 
					while(is_active != -1);*/


				}




			}

			std::cerr<<"sent complete\n";

			//conn->SendNow(body);

			std::string str = "";
			H.Chunkify(str);
			conn->SendNow(str);

			//conn->SendChunkNow(body);

			//sent[sent_index++] = resp.length();


			//std::cerr<<"\n after send\n";

			//std::cerr<<"the stats here are : cwnd="<<conn->stats->snd_cwnd<<" rtt="<<conn->stats->srtt<<"\n";




			//conn->stats->snd_cwnd = 0;
			//myfile << syscall(__NR_map_back, conn->sock, -1);
			//myfile.close();


			myCConn->inUse.unlock();

		}else{
			//unknown length
			H.SetHeader("X-UID", uid);
			H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
			conn->SendNow(H.BuildResponse("200", "OK"));
			//switch out the connection for an empty one - it makes no sense to keep these globally
			Socket::Connection * myConn = myCConn->conn;
			myCConn->conn = new Socket::Connection();
			myCConn->inUse.unlock();
			//continue sending data from this socket and keep it permanently in use
			while (myConn->connected() && conn->connected()){
				if (myConn->Received().size() || myConn->spool()){
					//forward any and all incoming data directly without parsing
					conn->SendNow(myConn->Received().get());
					myConn->Received().get().clear();
				}else{
					Util::sleep(30);
				}
			}
			myConn->close();
			delete myConn;
			conn->close();
		}
		return ret;
	}
}

///\brief Determines the type of connector to be used for handling a request.
///\param H The request to be handled..
///\return A string indicating the type of connector.
///Possible values are:
/// - "none" The request is not supported.
/// - "internal" The request should be handled by the proxy itself.
/// - "dynamic" The request should be dispatched to the HTTP Dynamic Connector
/// - "progressive" The request should be dispatched to the HTTP Progressive Connector
/// - "smooth" The request should be dispatched to the HTTP Smooth Connector
/// - "live" The request should be dispatched to the HTTP Live Connector
std::string proxyGetHandleType(HTTP::Parser & H){
	std::string url = H.getUrl();
	if (url.find("/dynamic/") != std::string::npos){
		std::string streamname = url.substr(9, url.find("/", 9) - 9);
		Util::Stream::sanitizeName(streamname);
		H.SetVar("stream", streamname);
		return "dynamic";
	}
	if (url.find("/smooth/") != std::string::npos && url.find(".ism") != std::string::npos){
		std::string streamname = url.substr(8, url.find("/", 8) - 12);
		Util::Stream::sanitizeName(streamname);
		H.SetVar("stream", streamname);
		return "smooth";
	}
	if (url.find("/hls/") != std::string::npos && (url.find(".m3u") != std::string::npos || url.find(".ts") != std::string::npos)){
		std::string streamname = url.substr(5, url.find("/", 5) - 5);
		Util::Stream::sanitizeName(streamname);
		H.SetVar("stream", streamname);
		return "live";
	}
	if (url.length() > 4){
		std::string ext = url.substr(url.length() - 4, 4);
		if (ext == ".flv" || ext == ".mp3"){
			std::string streamname = url.substr(1, url.length() - 5);
			Util::Stream::sanitizeName(streamname);
			H.SetVar("stream", streamname);
			return "progressive";
		}
		if (ext == ".ico"){
			return "internal";
		}
	}
	if (url == "/crossdomain.xml"){
		return "internal";
	}
	if (url == "/clientaccesspolicy.xml"){
		return "internal";
	}
	if (url.length() > 10 && url.substr(0, 7) == "/embed_" && url.substr(url.length() - 3, 3) == ".js"){
		return "internal";
	}
	if (url.length() > 9 && url.substr(0, 6) == "/info_" && url.substr(url.length() - 3, 3) == ".js"){
		return "internal";
	}
	return "none";
}

///\brief Function run as a thread to handle a single HTTP connection.
///\param pointer A Socket::Connection* indicating the connection to th client.
void proxyHandleHTTPConnection(void * pointer){
	Socket::Connection * conn = (Socket::Connection *)pointer;
	conn->setBlocking(false); //do not block on conn.spool() when no data is available
	HTTP::Parser Client;
	while (conn->connected()){
		if (conn->spool() || conn->Received().size()){
			//make sure it ends in a \n
			if ( *(conn->Received().get().rbegin()) != '\n'){
				std::string tmp = conn->Received().get();
				conn->Received().get().clear();
				if (conn->Received().size()){
					conn->Received().get().insert(0, tmp);
				}else{
					conn->Received().append(tmp);
				}
			}
			if (Client.Read(conn->Received().get())){
				std::string handler = proxyGetHandleType(Client);
#if DEBUG >= 4
				std::cout << "Received request: " << Client.getUrl() << " (" << conn->getSocket() << ") => " << handler << " (" << Client.GetVar("stream")
            																										  << ")" << std::endl;
				long long int startms = Util::getMS();
#endif
				long long int midms = 0;
				bool closeConnection = false;
				if (Client.GetHeader("Connection") == "close"){
					closeConnection = true;
				}

				if (handler == "none" || handler == "internal"){
					if (handler == "internal"){
						midms = proxyHandleInternal(Client, conn);
					}else{
						midms = proxyHandleUnsupported(Client, conn);
					}
				}else{
					midms = proxyHandleThroughConnector(Client, conn, handler);
				}
#if DEBUG >= 4
				long long int nowms = Util::getMS();
				std::cout << "Completed request " << conn->getSocket() << " " << handler << " in " << (midms - startms) << " ms (processing) / " << (nowms - midms) << " ms (transfer)" << std::endl;
#endif
				if (closeConnection){
					break;
				}
				Client.Clean(); //clean for any possible next requests
			}
		}else{
			Util::sleep(10); //sleep 10ms
		}
	}
	//close and remove the connection
	conn->close();
	delete conn;
}

} //Connector_HTTP namespace

int main(int argc, char ** argv){
	Util::Config conf(argv[0], PACKAGE_VERSION);
	conf.addConnectorOptions(8080);
	conf.parseArgs(argc, argv);
	Socket::Server server_socket = Socket::Server(conf.getInteger("listen_port"), conf.getString("listen_interface"));
	if ( !server_socket.connected()){
		return 1;
	}
	conf.activate();

	while (server_socket.connected() && conf.is_active){
		std::cerr<<"new connection\n";
		Socket::Connection S = server_socket.accept();

		//std::ofstream myfile  ("/home/sharvanath/mistserver/test.txt",std::ios::app);
		//struct sockaddr add;
		//socklen_t add_length=sizeof(struct sockaddr);
		//getsockname(incoming.sock, &add,&add_length);

		//myfile<<"hello1\n";
		//myfile << "the family here is : "<<add.sa_family<<"\n";
		//myfile.close();

		if (S.connected()){ //check if the new connection is valid
			//spawn a new thread for this connection
			tthread::thread T(Connector_HTTP::proxyHandleHTTPConnection, (void *)(new Socket::Connection(S)));
			//detach it, no need to keep track of it anymore
			T.detach();
		}else{
			Util::sleep(10); //sleep 10ms
		}
	} //while connected and not requested to stop
	server_socket.close();
	if (Connector_HTTP::timeouter){
		Connector_HTTP::timeouter->detach();
		delete Connector_HTTP::timeouter;
	}
	return 0;
} //main
