///\file conn_http_smooth.cpp
///\brief Contains the main code for the HTTP Smooth Connector

#include <iostream>
#include <iomanip>
#include <queue>
#include <sstream>

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <getopt.h>

#include <mist/socket.h>
#include <mist/http_parser.h>
#include <mist/json.h>
#include <mist/dtsc.h>
#include <mist/base64.h>
#include <mist/amf.h>
#include <mist/mp4.h>
#include <mist/config.h>
#include <mist/stream.h>
#include <mist/timing.h>

long long int binToInt(std::string & binary){
  long long int result = 0;
  for ( int i = 0; i < 8; i++){
    result <<= 8;
    result += binary[i];
  }
  return result;
}

std::string intToBin(long long int number){
  std::string result;
  result.resize(8);
  for( int i = 7; i >= 0; i--){
    result[i] = number & 0xFF;
    number >>= 8;
  }
  return result;
}

std::string toUTF16(std::string original){
  std::string result;
  result += (char)0xFF;
  result += (char)0xFE;
  for (std::string::iterator it = original.begin(); it != original.end(); it++){
    result += (*it);
    result += (char)0x00;
  }
  return result;
}

///\brief Holds everything unique to HTTP Connectors.
namespace Connector_HTTP {
  ///\brief Builds an index file for HTTP Smooth streaming.
  ///\param metadata The current metadata, used to generate the index.
  ///\return The index file for HTTP Smooth Streaming.
  std::string smoothIndex(DTSC::Meta & metadata){
    std::stringstream Result;
    Result << "<?xml version=\"1.0\" encoding=\"utf-16\"?>\n";
    Result << "<SmoothStreamingMedia "
              "MajorVersion=\"2\" "
              "MinorVersion=\"0\" "
              "TimeScale=\"10000000\" ";
    std::deque<std::map<int,DTSC::Track>::iterator> audioIters;
    std::deque<std::map<int,DTSC::Track>::iterator> videoIters;
    long long int maxWidth = 0;
    long long int maxHeight = 0;
    long long int minWidth = 99999999;
    long long int minHeight = 99999999;
    for (std::map<int,DTSC::Track>::iterator it = metadata.tracks.begin(); it != metadata.tracks.end(); it++){
      if (it->second.codec == "AAC"){
        audioIters.push_back(it);
      }
      if (it->second.type == "video" && it->second.codec == "H264"){
        videoIters.push_back(it);
        if (it->second.width > maxWidth){maxWidth = it->second.width;}
        if (it->second.width < minWidth){minWidth = it->second.width;}
        if (it->second.height > maxHeight){maxHeight = it->second.height;}
        if (it->second.height < minHeight){minHeight = it->second.height;}
      }
    }
    if (metadata.vod){
      Result << "Duration=\"" << (*videoIters.begin())->second.lastms << "0000\"";
    }else{
      Result << "Duration=\"0\" "
                "IsLive=\"TRUE\" "
                "LookAheadFragmentCount=\"2\" "
                "DVRWindowLength=\"" << metadata.bufferWindow << "0000\" "
                "CanSeek=\"TRUE\" "
                "CanPause=\"TRUE\" ";
    }
    Result << ">\n";

    //Add audio entries
    if (audioIters.size()){
      Result << "<StreamIndex "
                "Type=\"audio\" "
                "QualityLevels=\"" << audioIters.size() << "\" "
                "Name=\"audio\" "
                "Chunks=\"" << (*audioIters.begin())->second.keys.size() << "\" "
                "Url=\"Q({bitrate},{CustomAttributes})/A({start time})\">\n";
      int index = 0;
      for (std::deque<std::map<int,DTSC::Track>::iterator>::iterator it = audioIters.begin(); it != audioIters.end(); it++){
        Result << "<QualityLevel "
                  "Index=\"" << index << "\" "
                  "Bitrate=\"" << (*it)->second.bps * 8 << "\" "
                  "CodecPrivateData=\"" << std::hex;
        for (unsigned int i = 0; i < (*it)->second.init.size(); i++){
          Result << std::setfill('0') << std::setw(2) << std::right << (int)(*it)->second.init[i];
        }
        Result << std::dec << "\" "
                  "SamplingRate=\"" << (*it)->second.rate << "\" "
                  "Channels=\"2\" "
                  "BitsPerSample=\"16\" "
                  "PacketSize=\"4\" "
                  "AudioTag=\"255\" "
                  "FourCC=\"AACL\" >\n";
        Result << "<CustomAttributes>\n" 
                  "<Attribute Name = \"TrackID\" Value = \"" << (*it)->first << "\" />" 
                  "</CustomAttributes>";
        Result << "</QualityLevel>\n";
        index++;
      }
      if ((*audioIters.begin())->second.keys.size()){
        for (std::deque<DTSC::Key>::iterator it = (*audioIters.begin())->second.keys.begin(); it != (((*audioIters.begin())->second.keys.end()) - 1); it++){
          Result << "<c ";
          if (it == (*audioIters.begin())->second.keys.begin()){
            Result << "t=\"" << it->getTime() * 10000 << "\" ";
          }
          Result << "d=\"" << it->getLength() * 10000 << "\" />\n";
        }
      }
      Result << "</StreamIndex>\n";
    }
    //Add video entries
    if (videoIters.size()){
      Result << "<StreamIndex "
                "Type=\"video\" "
                "QualityLevels=\"" << videoIters.size() << "\" "
                "Name=\"video\" "
                "Chunks=\"" << (*videoIters.begin())->second.keys.size() << "\" "
                "Url=\"Q({bitrate},{CustomAttributes})/V({start time})\" "
                "MaxWidth=\"" << maxWidth << "\" "
                "MaxHeight=\"" << maxHeight << "\" "
                "DisplayWidth=\"" << maxWidth << "\" "
                "DisplayHeight=\"" << maxHeight << "\">\n";
      int index = 0;
      for (std::deque<std::map<int,DTSC::Track>::iterator>::iterator it = videoIters.begin(); it != videoIters.end(); it++){
        //Add video qualities
        Result << "<QualityLevel "
                  "Index=\"" << index << "\" "
                  "Bitrate=\"" << (*it)->second.bps * 8 << "\" "
                  "CodecPrivateData=\"" << std::hex;
        MP4::AVCC avccbox;
        avccbox.setPayload((*it)->second.init);
        std::string tmpString = avccbox.asAnnexB();
        for (unsigned int i = 0; i < tmpString.size(); i++){
          Result << std::setfill('0') << std::setw(2) << std::right << (int)tmpString[i];
        }
        Result << std::dec << "\" "
                  "MaxWidth=\"" << (*it)->second.width << "\" "
                  "MaxHeight=\"" << (*it)->second.height << "\" "
                  "FourCC=\"AVC1\" >\n";
        Result << "<CustomAttributes>\n" 
                  "<Attribute Name = \"TrackID\" Value = \"" << (*it)->first << "\" />" 
                  "</CustomAttributes>";
        Result << "</QualityLevel>\n";
        index++;
      }
      if ((*videoIters.begin())->second.keys.size()){
        for (std::deque<DTSC::Key>::iterator it = (*videoIters.begin())->second.keys.begin(); it != (((*videoIters.begin())->second.keys.end()) - 1); it++){
          Result << "<c ";
          if (it == (*videoIters.begin())->second.keys.begin()){
            Result << "t=\"" << it->getTime() * 10000 << "\" ";
          }
          Result << "d=\"" << it->getLength() * 10000 << "\" />\n";
        }
      }
      Result << "</StreamIndex>\n";
    }
    Result << "</SmoothStreamingMedia>\n";

#if DEBUG >= 8
    std::cerr << "Sending this manifest:" << std::endl << Result << std::endl;
#endif
    return toUTF16(Result.str());
  } //smoothIndex

  ///\brief Main function for the HTTP Smooth Connector
  ///\param conn A socket describing the connection the client.
  ///\return The exit code of the connector.
  int smoothConnector(Socket::Connection conn){
    std::deque<std::string> dataBuffer;//A buffer for the data that needs to be sent to the client.

    DTSC::Stream Strm;//Incoming stream buffer.
    HTTP::Parser HTTP_R;//HTTP Receiver
    HTTP::Parser HTTP_S;//HTTP Sender.

    bool ready4data = false;//Set to true when streaming is to begin.
    Socket::Connection ss( -1);//The Stream Socket, used to connect to the desired stream.
    std::string streamname;//Will contain the name of the stream.
    bool handlingRequest = false;

    bool wantsVideo = false;//Indicates whether this request is a video request.
    bool wantsAudio = false;//Indicates whether this request is an audio request.

    std::string Quality;//Indicates the request quality of the movie.
    long long int requestedTime = -1;//Indicates the fragment requested.
    std::string parseString;//A string used for parsing different aspects of the request.
    unsigned int lastStats = 0;//Indicates the last time that we have sent stats to the server socket.
    conn.setBlocking(false);//Set the client socket to non-blocking

    while (conn.connected()){
      if ( !handlingRequest){
        if (conn.spool() || conn.Received().size()){
          if (HTTP_R.Read(conn)){
  #if DEBUG >= 5
            std::cout << "Received request: " << HTTP_R.getUrl() << std::endl;
  #endif
            //Get data set by the proxy.
            conn.setHost(HTTP_R.GetHeader("X-Origin"));
            streamname = HTTP_R.GetHeader("X-Stream");
            if ( !ss){
              //initiate Stream Socket
              ss = Util::Stream::getStream(streamname);
              if ( !ss.connected()){
                #if DEBUG >= 1
                fprintf(stderr, "Could not connect to server!\n");
                #endif
                HTTP_S.Clean();
                HTTP_S.SetBody("No such stream is available on the system. Please try again.\n");
                conn.SendNow(HTTP_S.BuildResponse("404", "Not found"));
                ready4data = false;
                continue;
              }
              ss.setBlocking(false);
              Strm.waitForMeta(ss);
            }
      

            if (HTTP_R.url.find(".xap") != std::string::npos){
#include "xap.h"
              
              HTTP_S.Clean();
              HTTP_S.SetHeader("Content-Type", "application/siverlight");
              HTTP_S.SetHeader("Cache-Control", "cache");
              HTTP_S.SetBody("");
              HTTP_S.SetHeader("Content-Length", xap_len);
              HTTP_S.SendResponse("200", "OK", conn);
              conn.SendNow((const char *)xap_data, xap_len);
            }else{
              if (HTTP_R.url.find("Manifest") == std::string::npos){
                //We have a non-manifest request, parse it.
                
                Quality = HTTP_R.url.substr(HTTP_R.url.find("TrackID=", 8) + 8);
                Quality = Quality.substr(0, Quality.find(")"));
                parseString = HTTP_R.url.substr(HTTP_R.url.find(")/") + 2);
                wantsAudio = false;
                wantsVideo = false;
                if (parseString[0] == 'A'){
                  wantsAudio = true;
                }
                if (parseString[0] == 'V'){
                  wantsVideo = true;
                }
                parseString = parseString.substr(parseString.find("(") + 1);
                requestedTime = atoll(parseString.substr(0, parseString.find(")")).c_str());
                long long int selectedQuality = atoll(Quality.c_str());
                DTSC::Track & myRef = Strm.metadata.tracks[selectedQuality];
                if (Strm.metadata.live){
                  int seekable = Strm.canSeekms(requestedTime / 10000);
                  if (seekable == 0){
                    // iff the fragment in question is available, check if the next is available too
                    for (std::deque<DTSC::Key>::iterator it = myRef.keys.begin(); it != myRef.keys.end(); it++){
                      if (it->getTime() >= (requestedTime / 10000)){
                        if ((it + 1) == myRef.keys.end()){
                          seekable = 1;
                        }
                        break;
                      }
                    }
                  }
                  if (seekable < 0){
                    HTTP_S.Clean();
                    HTTP_S.SetBody("The requested fragment is no longer kept in memory on the server and cannot be served.\n");
                    conn.SendNow(HTTP_S.BuildResponse("412", "Fragment out of range"));
                    HTTP_R.Clean(); //clean for any possible next requests
                    std::cout << "Fragment @ " << requestedTime / 10000 << "ms too old (" << myRef.keys.begin()->getTime() << " - " << myRef.keys.rbegin()->getTime() << " ms)" << std::endl;
                    continue;
                  }
                  if (seekable > 0){
                    HTTP_S.Clean();
                    HTTP_S.SetBody("Proxy, re-request this in a second or two.\n");
                    conn.SendNow(HTTP_S.BuildResponse("208", "Ask again later"));
                    HTTP_R.Clean(); //clean for any possible next requests
                    std::cout << "Fragment @ " << requestedTime / 10000 << "ms not available yet (" << myRef.keys.begin()->getTime() << " - " << myRef.keys.rbegin()->getTime() << " ms)" << std::endl;
                    continue;
                  }
                }
                //Seek to the right place and send a play-once for a single fragment.
                std::stringstream sstream;
                
                long long mstime = 0;
                for (std::deque<DTSC::Key>::iterator it = myRef.keys.begin(); it != myRef.keys.end(); it++){
                  if (it->getTime() >= (requestedTime / 10000)){
                    mstime = it->getTime();
                    if (Strm.metadata.live){
                      if (it == myRef.keys.end() - 2){
                        HTTP_S.Clean();
                        HTTP_S.SetBody("Proxy, re-request this in a second or two.\n");
                        conn.SendNow(HTTP_S.BuildResponse("208", "Ask again later"));
                        HTTP_R.Clean(); //clean for any possible next requests
                        std::cout << "Fragment after fragment @ " << (requestedTime / 10000) << " not available yet" << std::endl;
                      }
                    }
                    break;
                  }
                }
                if (HTTP_R.url == "/"){continue;}//Don't continue, but continue instead.
                if (Strm.metadata.live){
                  if (mstime == 0 && (requestedTime / 10000) > 1){
                    HTTP_S.Clean();
                    HTTP_S.SetBody("The requested fragment is no longer kept in memory on the server and cannot be served.\n");
                    conn.SendNow(HTTP_S.BuildResponse("412", "Fragment out of range"));
                    HTTP_R.Clean(); //clean for any possible next requests
                    std::cout << "Fragment @ " << (requestedTime / 10000) << " too old" << std::endl;
                    continue;
                  }
                }
                
                //Obtain the corresponding track;
                DTSC::Track trackRef;
                for (std::map<int,DTSC::Track>::iterator it = Strm.metadata.tracks.begin(); it != Strm.metadata.tracks.end(); it++){
                  if (wantsVideo && it->second.codec == "H264"){
                    trackRef = it->second;
                  }
                  if (wantsAudio && it->second.codec == "AAC"){
                    trackRef = it->second;
                  }
                }
                //Also obtain the associated keyframe;
                DTSC::Key keyObj;
                int partOffset = 0;
                int keyDur = 0;
                for (std::deque<DTSC::Key>::iterator keyIt = trackRef.keys.begin(); keyIt != trackRef.keys.end(); keyIt++){
                  if (keyIt->getTime() >= (requestedTime / 10000)){
                    keyObj = (*keyIt);
                    std::deque<DTSC::Key>::iterator nextIt = keyIt;
                    nextIt++;
                    if (nextIt != trackRef.keys.end()){
                      keyDur = nextIt->getTime() - keyIt->getTime();
                    }else{
                      keyDur = -1;
                    }
                    break;
                  }
                  partOffset += keyIt->getParts();
                }
                
                sstream << "t " << myRef.trackID << "\n";
                sstream << "s " << keyObj.getTime() << "\n";
                if (keyDur != -1){
                  sstream << "p " << keyObj.getTime() + keyDur << "\n";
                }else{
                  sstream << "p\n";
                }

                ss.SendNow(sstream.str().c_str());
                
                //Wrap everything in mp4 boxes
                MP4::MFHD mfhd_box;
                mfhd_box.setSequenceNumber(((keyObj.getNumber() - 1) * 2) + myRef.trackID);
                
                MP4::TFHD tfhd_box;
                tfhd_box.setFlags(MP4::tfhdSampleFlag);
                tfhd_box.setTrackID(myRef.trackID);
                if (trackRef.type == "video"){
                  tfhd_box.setDefaultSampleFlags(0x00004001);
                }else{
                  tfhd_box.setDefaultSampleFlags(0x00008002);
                }
                
                MP4::TRUN trun_box;
                trun_box.setDataOffset(42);
                unsigned int keySize = 0;
                if (trackRef.type == "video"){
                 trun_box.setFlags(MP4::trundataOffset | MP4::trunfirstSampleFlags | MP4::trunsampleDuration | MP4::trunsampleSize | MP4::trunsampleOffsets);
                }else{
                  trun_box.setFlags(MP4::trundataOffset | MP4::trunsampleDuration | MP4::trunsampleSize);
                }
                trun_box.setFirstSampleFlags(0x00004002);
                for (int i = 0; i < keyObj.getParts(); i++){
                  MP4::trunSampleInformation trunSample;
                  trunSample.sampleSize = Strm.metadata.tracks[myRef.trackID].parts[i + partOffset].getSize();
                  keySize += Strm.metadata.tracks[myRef.trackID].parts[i + partOffset].getSize();
                  trunSample.sampleDuration = Strm.metadata.tracks[myRef.trackID].parts[i + partOffset].getDuration() * 10000;
                  if (trackRef.type == "video"){
                    trunSample.sampleOffset = Strm.metadata.tracks[myRef.trackID].parts[i + partOffset].getOffset() * 10000;
                  }
                  trun_box.setSampleInformation(trunSample, i);
                }
                
                MP4::SDTP sdtp_box;
                sdtp_box.setVersion(0);
                if (trackRef.type == "video"){
                  sdtp_box.setValue(36, 4);
                  for (int i = 1; i < keyObj.getParts(); i++){
                    sdtp_box.setValue(20, 4 + i);
                  }
                }else{
                  sdtp_box.setValue(40, 4);
                  for (int i = 1; i < keyObj.getParts(); i++){
                    sdtp_box.setValue(40, 4 + i);
                  }
                }
                
                MP4::TRAF traf_box;
                traf_box.setContent(tfhd_box, 0);
                traf_box.setContent(trun_box, 1);
                traf_box.setContent(sdtp_box, 2);
                
                //If the stream is live, we want to have a fragref box if possible
                if (Strm.metadata.live){
                  MP4::UUID_TrackFragmentReference fragref_box;
                  fragref_box.setVersion(1);
                  fragref_box.setFragmentCount(0);
                  int fragCount = 0;
                  for (unsigned int i = 0; fragCount < 2 && i < trackRef.keys.size() - 1; i++){
                    if (trackRef.keys[i].getTime() > (requestedTime / 10000)){
                      fragref_box.setTime(fragCount, trackRef.keys[i].getTime() * 10000);
                      fragref_box.setDuration(fragCount, trackRef.keys[i].getLength() * 10000);
                      fragref_box.setFragmentCount(++fragCount);
                    }
                  }
                  traf_box.setContent(fragref_box, 3);
                }

                MP4::MOOF moof_box;
                moof_box.setContent(mfhd_box, 0);
                moof_box.setContent(traf_box, 1);
                //Setting the correct offsets.
                trun_box.setDataOffset(moof_box.boxedSize() + 8);
                traf_box.setContent(trun_box, 1);
                moof_box.setContent(traf_box, 1);

                HTTP_S.Clean();
                HTTP_S.SetHeader("Content-Type", "video/mp4");
                HTTP_S.SetHeader("Pragma", "IISMS/5.0,IIS Media Services Premium by Microsoft");
                HTTP_S.SetHeader("ETag", "3b517e5a0586303");
                HTTP_S.StartResponse(HTTP_R, conn);
                HTTP_S.Chunkify(moof_box.asBox(), moof_box.boxedSize(), conn);
                int size = htonl(keySize + 8);
                HTTP_S.Chunkify((char*)&size, 4, conn);
                HTTP_S.Chunkify("mdat", 4, conn);
                handlingRequest = true;
              }else{
                //We have a request for a Manifest, generate and send it.
                
                HTTP_S.Clean();
                HTTP_S.SetHeader("Content-Type", "text/xml");
                HTTP_S.SetHeader("Cache-Control", "no-cache");
                std::string manifest = smoothIndex(Strm.metadata);
                HTTP_S.SetBody(manifest);
                HTTP_S.SendResponse("200", "OK", conn);
              }
            }
            //Clean for any possible next requests
            HTTP_R.Clean();
          }
          
        }else{
          //Wait 250ms before checking for new data.
          Util::sleep(250);
        }
      }else{
        if (!ready4data){
          //Wait 250ms before checking for new data.
          Util::sleep(250);
        }
      }
      if (ready4data){
        unsigned int now = Util::epoch();
        if (now != lastStats){
          //Send new stats.
          lastStats = now;
          ss.SendNow(conn.getStats("HTTP_Smooth"));
        }
        if (ss.spool()){
          while (Strm.parsePacket(ss.Received())){
            if (Strm.lastType() == DTSC::AUDIO || Strm.lastType() == DTSC::VIDEO){
              HTTP_S.Chunkify(Strm.lastData(), conn);
            }
            if (Strm.lastType() == DTSC::PAUSEMARK){
              HTTP_S.Chunkify("", 0, conn);
              handlingRequest = false;
            }
          }
        }else{
          Util::sleep(10);
        }
        if ( !ss.connected()){
          break;
        }
      }
    }
    conn.close();
    ss.SendNow(conn.getStats("HTTP_Smooth").c_str());
    ss.close();
    return 0;
  }//Smooth_Connector main function

}//Connector_HTTP namespace

///\brief The standard process-spawning main function.
int main(int argc, char ** argv){
  Util::Config conf(argv[0], PACKAGE_VERSION);
  JSON::Value capa;
  capa["desc"] = "Enables HTTP protocol Microsoft-specific smooth streaming through silverlight (also known as HSS).";
  capa["deps"] = "HTTP";
  capa["url_rel"] = "/smooth/$.ism/Manifest";
  capa["url_prefix"] = "/smooth/$.ism/";
  capa["socket"] = "http_smooth";
  capa["codecs"][0u][0u].append("H264");
  capa["codecs"][0u][1u].append("AAC");
  capa["methods"][0u]["handler"] = "http";
  capa["methods"][0u]["type"] = "html5/application/vnd.ms-ss";
  capa["methods"][0u]["priority"] = 9ll;
  capa["methods"][0u]["nolive"] = 1;
  capa["methods"][1u]["handler"] = "http";
  capa["methods"][1u]["type"] = "silverlight";
  capa["methods"][1u]["priority"] = 1ll;
  capa["methods"][1u]["nolive"] = 1;
  conf.addBasicConnectorOptions(capa);
  conf.parseArgs(argc, argv);
  
  if (conf.getBool("json")){
    std::cout << capa.toString() << std::endl;
    return -1;
  }

  Socket::Server server_socket = Socket::Server(Util::getTmpFolder() + capa["socket"].asStringRef());
  if ( !server_socket.connected()){
    return 1;
  }
  conf.activate();

  while (server_socket.connected() && conf.is_active){
    Socket::Connection S = server_socket.accept();
    if (S.connected()){ //check if the new connection is valid
      pid_t myid = fork();
      if (myid == 0){ //if new child, start MAINHANDLER
        return Connector_HTTP::smoothConnector(S);
      }else{ //otherwise, do nothing or output debugging text
#if DEBUG >= 5
        fprintf(stderr, "Spawned new process %i for socket %i\n", (int)myid, S.getSocket());
#endif
      }
    }
  } //while connected
  server_socket.close();
  return 0;
} //main
