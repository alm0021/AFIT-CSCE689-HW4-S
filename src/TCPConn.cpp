#include <stdexcept>
#include <strings.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>
#include "TCPConn.h"
#include "strfuncts.h"
#include <crypto++/secblock.h>
#include <crypto++/osrng.h>
#include <crypto++/filters.h>
#include <crypto++/rijndael.h>
#include <crypto++/gcm.h>
#include <crypto++/aes.h>

using namespace CryptoPP;

// Common defines for this TCPConn
const unsigned int iv_size = AES::BLOCKSIZE;
const unsigned int key_size = AES::DEFAULT_KEYLENGTH;
const unsigned int auth_size = 16;

/**********************************************************************************************
 * TCPConn (constructor) - creates the connector and initializes - creates the command strings
 *                         to wrap around network commands
 *
 *    Params: key - reference to the pre-loaded AES key
 *            verbosity - stdout verbosity - 3 = max
 *
 **********************************************************************************************/

TCPConn::TCPConn(LogMgr &server_log, CryptoPP::SecByteBlock &key, unsigned int verbosity) : _data_ready(false),
                                                                                            _aes_key(key),
                                                                                            _verbosity(verbosity),
                                                                                            _server_log(server_log)
{
   // prep some tools to search for command sequences in data
   uint8_t slash = (uint8_t)'/';
   c_rep.push_back((uint8_t)'<');
   c_rep.push_back((uint8_t)'R');
   c_rep.push_back((uint8_t)'E');
   c_rep.push_back((uint8_t)'P');
   c_rep.push_back((uint8_t)'>');

   c_endrep = c_rep;
   c_endrep.insert(c_endrep.begin() + 1, 1, slash);

   c_ack.push_back((uint8_t)'<');
   c_ack.push_back((uint8_t)'A');
   c_ack.push_back((uint8_t)'C');
   c_ack.push_back((uint8_t)'K');
   c_ack.push_back((uint8_t)'>');

   c_auth.push_back((uint8_t)'<');
   c_auth.push_back((uint8_t)'A');
   c_auth.push_back((uint8_t)'U');
   c_auth.push_back((uint8_t)'T');
   c_auth.push_back((uint8_t)'>');

   c_endauth = c_auth;
   c_endauth.insert(c_endauth.begin() + 1, 1, slash);

   c_sid.push_back((uint8_t)'<');
   c_sid.push_back((uint8_t)'S');
   c_sid.push_back((uint8_t)'I');
   c_sid.push_back((uint8_t)'D');
   c_sid.push_back((uint8_t)'>');

   c_endsid = c_sid;
   c_endsid.insert(c_endsid.begin() + 1, 1, slash);
}

TCPConn::~TCPConn()
{
}

/**********************************************************************************************
 * accept - simply calls the acceptFD FileDesc method to accept a connection on a server socket.
 *
 *    Params: server - an open/bound server file descriptor with an available connection
 *
 *    Throws: socket_error for recoverable errors, runtime_error for unrecoverable types
 **********************************************************************************************/

bool TCPConn::accept(SocketFD &server)
{
   // Accept the connection
   bool results = _connfd.acceptFD(server);

   // Set the state as waiting for the authorization packet
   _status = s_connected;
   _connected = true;
   return results;
}

/**********************************************************************************************
 * sendData - sends the data in the parameter to the socket
 *
 *    Params:  msg - the string to be sent
 *             size - if we know how much data we should expect to send, this should be populated
 *
 *    Throws: runtime_error for unrecoverable errors
 **********************************************************************************************/

bool TCPConn::sendData(std::vector<uint8_t> &buf)
{

   _connfd.writeBytes<uint8_t>(buf);

   return true;
}

/**********************************************************************************************
 * sendEncryptedData - sends the data in the parameter to the socket after block encrypting it
 *
 *    Params:  msg - the string to be sent
 *             size - if we know how much data we should expect to send, this should be populated
 *
 *    Throws: runtime_error for unrecoverable errors
 **********************************************************************************************/

bool TCPConn::sendEncryptedData(std::vector<uint8_t> &buf)
{

   // Encrypt
   encryptData(buf);

   // And send!
   return sendData(buf);
}

/**********************************************************************************************
 * encryptData - block encrypts data and places the results in the buffer in <ID><Data> format
 *
 *    Params:  buf - where to place the <IV><Data> stream
 *
 *    Throws: runtime_error for unrecoverable errors
 **********************************************************************************************/

void TCPConn::encryptData(std::vector<uint8_t> &buf)
{
   // For the initialization vector
   SecByteBlock init_vector(iv_size);
   AutoSeededRandomPool rnd;

   // Generate our random init vector
   rnd.GenerateBlock(init_vector, init_vector.size());

   // Encrypt the data
   CFB_Mode<AES>::Encryption encryptor;
   encryptor.SetKeyWithIV(_aes_key, _aes_key.size(), init_vector);

   std::string cipher;
   ArraySource as(buf.data(), buf.size(), true,
                  new StreamTransformationFilter(encryptor, new StringSink(cipher)));

   // Now add the IV to the stream we will be sending out
   std::vector<uint8_t> enc_data(init_vector.begin(), init_vector.end());
   enc_data.insert(enc_data.end(), cipher.begin(), cipher.end());
   buf = enc_data;
}

/**********************************************************************************************
 * handleConnection - performs a check of the connection, looking for data on the socket and
 *                    handling it based on the _status, or stage, of the connection
 *
 *    Throws: runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::handleConnection()
{

   try
   {
      switch (_status)
      {

      // Client (1): Just connected, send our SID (A)
      case s_connecting:
         sendSID();
         break;

      // Server (1): Wait for the SID from a newly-connected client, then send our SID
      case s_connected:
         waitForSID();
         break;

      //Server (1,2): Send unencrypted random string (R_B)
      case s_S_sendR_B:
         //message2
         sendRand_B();
         break;

      //Client (2): Wait for R_B and send back server's encrypted random string (K(R_B))
      case s_C_waitR_B:
         //challenge2
         sendEncrRand_B();
         break;

      //Client (4): Send unencrypted random string (R_A)
      case s_C_sendR_A:
         //challenge4
         sendRand_A();
         break;

      //Server (3): Decrypt random string (K(R_B)) and send back client's encrypted random string (K(R_A))
      case s_S_waitKR_B:
         //challenge5
         waitEncrRand_B();
         break;
      
      //Server (4,5): Waits for R_A and sends K(R_A) to client
      case s_S_waitR_A:
         waitRand_A();
         break;

      //Client (5): Decrypt encrypted random string (K(R_A)) and authorize server
      case s_C_waitKR_A:
         client_auth();
         break;

      // Client: connecting user - replicate data
      case s_datatx:
         transmitData();
         break;

      // Server: Receive data from the client
      case s_datarx:
         waitForData();
         break;

      // Client: Wait for acknowledgement that data sent was received before disconnecting
      case s_waitack:
         awaitAck();
         break;

      // Server: Data received and conn disconnected, but waiting for the data to be retrieved
      case s_hasdata:
         break;

      default:
         throw std::runtime_error("Invalid connection status!");
         break;
      }
   }
   catch (socket_error &e)
   {
      std::cout << "Socket error, disconnecting.\n";
      disconnect();
      return;
   }
}

/**********************************************************************************************
 * sendRand_B()  - Server (1,2): after receiving SID from client, sends unencrypted random string
 *    to client
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/
void TCPConn::sendRand_B()
{

   // If data on the socket, should be our Auth string from our host server
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      if (!getCmdData(buf, c_sid, c_endsid))
      {
         std::stringstream msg;
         msg << "SID string from connecting client invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      std::string rand_B(_authstr);
      genRandString(rand_B, auth_size);
      std::vector<uint8_t> rbuf(rand_B.begin(), rand_B.end());
      wrapCmd(rbuf, c_auth, c_endauth);
      sendData(rbuf);

      //_status = s_datarx;
      _status = s_S_waitKR_B;
   }

}

/**********************************************************************************************
 * sendEncrRand_B  - Client (2,3): after receiving random string from server, sends encrypted random string
 *    to server
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/
void TCPConn::sendEncrRand_B()
{
   //receive random string from server (R_B)
   //encrypt random string (K(R_B))
   //send K(R_B) to server

   // If data on the socket, should be random string (R_B) from our host server
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      if (!getCmdData(buf, c_auth, c_endauth))
      {
         std::stringstream msg;
         msg << "Random string from connecting client invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      //encrypt and send to server
      if (!sendEncryptedData(buf))
      {
         std::stringstream msg;
         msg << "Error encrypting and sending data to server. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      _status = s_C_sendR_A;
   }
}

/**********************************************************************************************
 * sendRand_A()  - Client (4): after sending K(R_B) to server, sends unencrypted random string
 *    to server
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/
void TCPConn::sendRand_A()
{

   std::string rand_A = _authstr;
   genRandString(rand_A, auth_size);
   std::vector<uint8_t> buf(rand_A.begin(), rand_A.end());
   wrapCmd(buf, c_auth, c_endauth);
   sendData(buf);

   _status = s_C_waitKR_A;
}

/**********************************************************************************************
 * waitEncrRand_B  - Server (3): after client sends K(R_B), server receives K(R_B) and decrypts
 * 
 **********************************************************************************************/
void TCPConn::waitEncrRand_B()
{
   // If data on the socket, should be encrypted random string from our client
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      //Get K(R_B) and decrypt
      if (!getEncryptedData(buf))
         return;

      if (!getCmdData(buf, c_auth, c_endauth))
      {
         std::stringstream msg;
         msg << "Encrypted random string from connecting client invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      //check if decrypted K(R_B) is equal to R_B
      if (_authstr.compare(toString(buf)) != 0)
      {
         std::stringstream msg;
         msg << "Client shared key not valid. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      //set status to wait for R_A
      _status = s_S_waitR_A;
   }
}

/**********************************************************************************************
 * waitRand_A  - Server (4,5): after server receives K(R_B), server receives R_A from client, 
 *    encrypts, send K(R_A) to client
 * 
 **********************************************************************************************/
void TCPConn::waitRand_A()
{
   // If data on the socket, should be random string (R_A) from our host server
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      // Get R_A
      if (!getCmdData(buf, c_auth, c_endauth))
      {
         std::stringstream msg;
         msg << "Random string from connecting client invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      //encrypt R_A and send K(R_A) to server
      if (!sendEncryptedData(buf))
      {
         std::stringstream msg;
         msg << "Error encrypting and sending data to server. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      //set status to wait for some message from client confirming authentication
      _status = s_connected;
   }
}

/**********************************************************************************************
 * client_auth  - Client (5): Decrypts K(R_A) and establishes trust with server
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/
void TCPConn::client_auth()
{

   //receive encrypted random string (K(R_B))
   //receive random string from server (R_A)
   //encrypt R_A and send K(R_A) to server

   // If data on the socket, should be encrypted random string from server
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;
      std::vector<uint8_t> buf2;

      //Get K(R_B) and decrypt
      if (!getEncryptedData(buf))
         return;

      if (!getCmdData(buf, c_auth, c_endauth))
      {
         std::stringstream msg;
         msg << "Encrypted random string from connecting server invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }
      //check if decrypted K(R_B) is equal to R_B
      if (_authstr.compare(toString(buf)) != 0)
      {
         std::stringstream msg;
         msg << "Server shared key not valid. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      //Resend SID as confirmation of authentication
      std::vector<uint8_t> sbuf(_svr_id.begin(), _svr_id.end());
      wrapCmd(sbuf, c_sid, c_endsid);
      sendData(sbuf);
      
      _status = s_datatx;
   }
}

/**********************************************************************************************
 * sendSID()  - Client: after a connection, client sends its Server ID to the server
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::sendSID()
{
   std::vector<uint8_t> buf(_svr_id.begin(), _svr_id.end());
   wrapCmd(buf, c_sid, c_endsid);
   sendData(buf);

   //_status = s_datatx;
   _status = s_C_waitR_B;
}

/**********************************************************************************************
 * waitForSID()  - receives the SID and sends our SID
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::waitForSID()
{

   // If data on the socket, should be our Auth string from our host server
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      if (!getCmdData(buf, c_sid, c_endsid))
      {
         std::stringstream msg;
         msg << "SID string from connecting client invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      std::string node(buf.begin(), buf.end());
      setNodeID(node.c_str());

      // Send our Node ID
      buf.assign(_svr_id.begin(), _svr_id.end());
      wrapCmd(buf, c_sid, c_endsid);
      sendData(buf);

      _status = s_datarx;
   }
}

/**********************************************************************************************
 * transmitData()  - receives the SID from the server and transmits data
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::transmitData()
{

   // If data on the socket, should be our Auth string from our host server
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      if (!getCmdData(buf, c_sid, c_endsid))
      {
         std::stringstream msg;
         msg << "SID string from connected server invalid format. Cannot authenticate.";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      std::string node(buf.begin(), buf.end());
      setNodeID(node.c_str());

      // Send the replication data
      sendData(_outputbuf);

      if (_verbosity >= 3)
         std::cout << "Successfully authenticated connection with " << getNodeID() << " and sending replication data.\n";

      // Wait for their response
      _status = s_waitack;
   }
}

/**********************************************************************************************
 * waitForData - receiving server, authentication complete, wait for replication datai
               Also sends a plaintext random auth string of our own
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::waitForData()
{

   // If data on the socket, should be replication data
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      if (!getCmdData(buf, c_rep, c_endrep))
      {
         std::stringstream msg;
         msg << "Replication data possibly corrupted from" << getNodeID() << "\n";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return;
      }

      // Got the data, save it
      _inputbuf = buf;
      _data_ready = true;

      // Send the acknowledgement and disconnect
      sendData(c_ack);

      if (_verbosity >= 2)
         std::cout << "Successfully received replication data from " << getNodeID() << "\n";

      disconnect();
      _status = s_hasdata;
   }
}

/**********************************************************************************************
 * awaitAwk - waits for the awk that data was received and disconnects
 *
 *    Throws: socket_error for network issues, runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::awaitAck()
{

   // Should have the awk message
   if (_connfd.hasData())
   {
      std::vector<uint8_t> buf;

      if (!getData(buf))
         return;

      if (findCmd(buf, c_ack) == buf.end())
      {
         std::stringstream msg;
         msg << "Awk expected from data send, received something else. Node:" << getNodeID() << "\n";
         _server_log.writeLog(msg.str().c_str());
      }

      if (_verbosity >= 3)
         std::cout << "Data ack received from " << getNodeID() << ". Disconnecting.\n";

      disconnect();
   }
}

/**********************************************************************************************
 * getData - Reads in data from the socket and checks to see if there's an end command to the
 *           message to confirm we got it all
 *
 *    Params: None - data is stored in _inputbuf for retrieval with GetInputData
 *
 *    Returns: true if the data is ready to be read, false if they lost connection
 *
 *    Throws: runtime_error for unrecoverable issues
 **********************************************************************************************/

bool TCPConn::getData(std::vector<uint8_t> &buf)
{

   std::vector<uint8_t> readbuf;
   size_t count = 0;

   buf.clear();

   while (_connfd.hasData())
   {
      // read the data on the socket up to 1024
      count += _connfd.readBytes<uint8_t>(readbuf, 1024);

      // check if we lost connection
      if (readbuf.size() == 0)
      {
         std::stringstream msg;
         std::string ip_addr;
         msg << "Connection from server " << _node_id << " lost (IP: " << getIPAddrStr(ip_addr) << ")";
         _server_log.writeLog(msg.str().c_str());
         disconnect();
         return false;
      }

      buf.insert(buf.end(), readbuf.begin(), readbuf.end());

      // concat the data onto anything we've read before
      //      _inputbuf.insert(_inputbuf.end(), readbuf.begin(), readbuf.end());
   }
   return true;
}

/**********************************************************************************************
 * decryptData - Takes in an encrypted buffer in the form IV/Data and decrypts it, replacing
 *               buf with the decrypted info (destroys IV string>
 *
 *    Params: buf - the encrypted string and holds the decrypted data (minus IV)
 *
 **********************************************************************************************/
void TCPConn::decryptData(std::vector<uint8_t> &buf)
{
   // For the initialization vector
   SecByteBlock init_vector(iv_size);

   // Copy the IV from the incoming stream of data
   init_vector.Assign(buf.data(), iv_size);
   buf.erase(buf.begin(), buf.begin() + iv_size);

   // Decrypt the data
   CFB_Mode<AES>::Decryption decryptor;
   decryptor.SetKeyWithIV(_aes_key, _aes_key.size(), init_vector);

   std::string recovered;
   ArraySource as(buf.data(), buf.size(), true,
                  new StreamTransformationFilter(decryptor, new StringSink(recovered)));

   buf.assign(recovered.begin(), recovered.end());
}

/**********************************************************************************************
 * getEncryptedData - Reads in data from the socket and decrypts it, passing the decrypted
 *                    data back in buf
 *
 *    Params: None - data is stored in _inputbuf for retrieval with GetInputData
 *
 *    Returns: true if the data is ready to be read, false otherwise
 *
 *    Throws: runtime_error for unrecoverable issues
 **********************************************************************************************/

bool TCPConn::getEncryptedData(std::vector<uint8_t> &buf)
{
   // Get the data from the socket
   if (!getData(buf))
      return false;

   decryptData(buf);

   return true;
}

/**********************************************************************************************
 * findCmd - returns an iterator to the location of a string where a command starts
 * hasCmd - returns true if command was found, false otherwise
 *
 *    Params: buf = the data buffer to look for the command within
 *            cmd - the command string to search for in the data
 *
 *    Returns: iterator - points to cmd position if found, end() if not found
 *
 **********************************************************************************************/

std::vector<uint8_t>::iterator TCPConn::findCmd(std::vector<uint8_t> &buf, std::vector<uint8_t> &cmd)
{
   return std::search(buf.begin(), buf.end(), cmd.begin(), cmd.end());
}

bool TCPConn::hasCmd(std::vector<uint8_t> &buf, std::vector<uint8_t> &cmd)
{
   return !(findCmd(buf, cmd) == buf.end());
}

/**********************************************************************************************
 * getCmdData - looks for a startcmd and endcmd and returns the data between the two 
 *
 *    Params: buf = the string to search for the tags
 *            startcmd - the command at the beginning of the data sought
 *            endcmd - the command at the end of the data sought
 *
 *    Returns: true if both start and end commands were found, false otherwisei
 *
 **********************************************************************************************/

bool TCPConn::getCmdData(std::vector<uint8_t> &buf, std::vector<uint8_t> &startcmd,
                         std::vector<uint8_t> &endcmd)
{
   std::vector<uint8_t> temp = buf;
   auto start = findCmd(temp, startcmd);
   auto end = findCmd(temp, endcmd);

   if ((start == temp.end()) || (end == temp.end()) || (start == end))
      return false;

   buf.assign(start + startcmd.size(), end);
   return true;
}

/**********************************************************************************************
 * wrapCmd - wraps the command brackets around the passed-in data
 *
 *    Params: buf = the string to wrap around
 *            startcmd - the command at the beginning of the data
 *            endcmd - the command at the end of the data
 *
 **********************************************************************************************/

void TCPConn::wrapCmd(std::vector<uint8_t> &buf, std::vector<uint8_t> &startcmd,
                      std::vector<uint8_t> &endcmd)
{
   std::vector<uint8_t> temp = startcmd;
   temp.insert(temp.end(), buf.begin(), buf.end());
   temp.insert(temp.end(), endcmd.begin(), endcmd.end());

   buf = temp;
}

/**********************************************************************************************
 * getReplData - Returns the data received on the socket and marks the socket as done
 *
 *    Params: buf = the data received
 *
 *
 *    Throws: runtime_error for unrecoverable issues
 **********************************************************************************************/

void TCPConn::getInputData(std::vector<uint8_t> &buf)
{

   // Returns the replication data off this connection, then prepares it to be removed
   buf = _inputbuf;

   _data_ready = false;
   _status = s_none;
}

/**********************************************************************************************
 * connect - Opens the socket FD, attempting to connect to the remote server
 *
 *    Params:  ip_addr - ip address string to connect to
 *             port - port in host format to connect to
 *
 *    Throws: socket_error exception if failed. socket_error is a child class of runtime_error
 **********************************************************************************************/

void TCPConn::connect(const char *ip_addr, unsigned short port)
{

   // Set the status to connecting
   _status = s_connecting;

   // Try to connect
   if (!_connfd.connectTo(ip_addr, port))
      throw socket_error("TCP Connection failed!");

   _connected = true;
}

// Same as above, but ip_addr and port are in network (big endian) format
void TCPConn::connect(unsigned long ip_addr, unsigned short port)
{
   // Set the status to connecting
   _status = s_connecting;

   if (!_connfd.connectTo(ip_addr, port))
      throw socket_error("TCP Connection failed!");

   _connected = true;
}

/**********************************************************************************************
 * assignOutgoingData - sets up the connection so that, at the next handleConnection, the data
 *                      is sent to the target server
 *
 *    Params:  data - the data stream to send to the server
 *
 **********************************************************************************************/

void TCPConn::assignOutgoingData(std::vector<uint8_t> &data)
{

   _outputbuf.clear();
   _outputbuf = c_rep;
   _outputbuf.insert(_outputbuf.end(), data.begin(), data.end());
   _outputbuf.insert(_outputbuf.end(), c_endrep.begin(), c_endrep.end());
}

/**********************************************************************************************
 * disconnect - cleans up the socket as required and closes the FD
 *
 *    Throws: runtime_error for unrecoverable issues
 **********************************************************************************************/
void TCPConn::disconnect()
{
   _connfd.closeFD();
   _connected = false;
}

/**********************************************************************************************
 * isConnected - performs a simple check on the socket to see if it is still open 
 *
 *    Throws: runtime_error for unrecoverable issues
 **********************************************************************************************/
bool TCPConn::isConnected()
{
   return _connected;
   // return _connfd.isOpen(); // This does not work very well
}

/**********************************************************************************************
 * getIPAddrStr - gets a string format of the IP address and loads it in buf
 *
 **********************************************************************************************/
const char *TCPConn::getIPAddrStr(std::string &buf)
{
   _connfd.getIPAddrStr(buf);
   return buf.c_str();
}
