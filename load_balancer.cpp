#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"

#include <ctime>
using namespace std;


// ---- Constants ----
const char* SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue{


  

    
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front, * rear;
    int ct;


public:
    // Implement these functions:
    Queue(){
    
        ct = 0;
        front = nullptr;
        rear = nullptr;

    }

    ~Queue(){
    

        while (front != nullptr){
            Node* temp = front;
            front = front->next;
            delete temp;

        }

        rear = nullptr;
        ct = 0;

    }
    void enqueue(const T& val){
    

        Node* n = new Node;
        n->data = val;
        n->next = nullptr;

        if (rear == nullptr){
            front = n;
        }

        else{
        
            rear->next = n;
        }

        rear = n;
        ct++;


    }

    T dequeue(){

        if (front == nullptr){
        
            return T{};
        }

        Node* temp = front;
        T value = temp->data;

        front = front->next;
        delete temp;
        ct--;

        if (front == nullptr){
            rear = nullptr;
        }

        return value;

    }

    bool isEmpty(){
        return ct == 0;

    }

    int size(){

        return ct;
    }
};


struct ClientInfo{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n){
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.

    size_t total_Sent = 0;

    while (total_Sent < n){
        ssize_t sent = send(sock, buf + total_Sent, n - total_Sent, 0);

        if (sent <= 0){
            return false;
        }

        total_Sent += sent;
    }

    return true;



}


uint32_t calculateChecksum(uint32_t crc, const char* data, size_t length);

bool sendError(int sock, const string& message){
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 

    char status = 1;

    uint32_t crc = 0xFFFFFFFF;
    crc = calculateChecksum(crc, &status, 1);
    crc = calculateChecksum(crc, message.c_str(), message.size());

    crc ^= 0xFFFFFFFF;

    if (!sendAll(sock, &status, 1)) {
        return false;
    }

    if (!sendAll(sock, message.c_str(), message.size())) {
        return false;
    }

    char checksum[4];

    for (int i = 0; i < 4; i++) {
        checksum[i] = (crc >> (8 * i)) & 0xFF;
    }

    if (!sendAll(sock, checksum, 4)) {
        return false;
    }

    return true;

}

int createListenSocket(int port){
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (sock < 0){

        perror("err, creating socket failed");
        exit(1);
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0){
        perror("err, bind failed");
        close(sock);
        exit(1);
    }

    if (listen(sock, LISTEN_BACKLOG) < 0){

        perror("err, listen failed");
        close(sock);
        exit(1);
    }

    return sock;

}

int connectToServer(const char* host, int port){
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 


    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (sock < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0){
        close(sock);
        return -1;

    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0){

        close(sock);
        return -1;
    }

    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port){
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.


    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    int r = system(command.c_str());

    return r == 0;
}

int connectToServerWithWakeup(){
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.

    int sock = connectToServer(serverHost.c_str(), serverPort);

    if (sock >= 0) {
        return sock;
    }

    if (!startServer(serverPort)) {
        return -1;
    }

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++){
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);


        if (sock >= 0) {
            return sock;
        }
    }

    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


uint32_t calculateChecksum(uint32_t crc, const char* data, size_t length){
    for (size_t i = 0; i < length; i++){
        crc ^= (unsigned char)data[i];

        for (int j = 0; j < 8; j++){
        
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

void relayOneClient(ClientInfo& c){
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
  
  
    int serverSock = connectToServerWithWakeup();

    if (serverSock == -1){
        sendError(c.sock, "err,backend server failed to start");
        close(c.sock);
        return;
    }


    char buffer[IO_BUFFER_SIZE];
    TrailerChecker uploadChecker;
    bool uploadFailed = false;

    while (true){
        ssize_t received = recv(c.sock, buffer, sizeof(buffer), 0);

        if (received < 0){
        
            uploadFailed = true;
            break;
        }

        if (received == 0) {
            break;
        }

        string safeData = uploadChecker.addData(buffer, received);

        if (!safeData.empty()){
        
            if (!sendAll(serverSock, safeData.c_str(), safeData.size())){
           
                uploadFailed = true;
                break;
            }
        }
    }

    if (uploadFailed || !uploadChecker.verify()){
    
        if (!uploadFailed) {
            sendError(c.sock, "err, checksum mismatch, upload corrupted");

        }

        close(serverSock);
        close(c.sock);
        return;
    }

    string uploadTrailer = uploadChecker.getTrailer();

    if (!sendAll(serverSock, uploadTrailer.c_str(), uploadTrailer.size())){
        close(serverSock);
        close(c.sock);
        return;

    }

    shutdown(serverSock, SHUT_WR);
    
    TrailerChecker responseChecker;
    bool responseFailed = false;

    while (true){
        ssize_t received = recv(serverSock, buffer, sizeof(buffer), 0);

        if (received < 0){
        
            responseFailed = true;
            break;
        }

        if (received == 0) {
            break;
        }

        string safeData = responseChecker.addData(buffer, received);

        if (!safeData.empty()){
            if (!sendAll(c.sock, safeData.c_str(), safeData.size())){
            
                responseFailed = true;
                break;
            }
        }
    }

    if (!responseFailed && responseChecker.verify()){
        string responseTrailer = responseChecker.getTrailer();
        sendAll(c.sock, responseTrailer.c_str(), responseTrailer.size());
    }


    close(serverSock);
    close(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock){
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).

    while (flag){
        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);


        int clientSock = accept(
            listenSock,
            (sockaddr*)&clientAddr,
            &addrLen
        );

        if (clientSock < 0){
            if (!flag) {

                break;
            }

            perror("Accept failed");
            continue;
        }

        ClientInfo client;
        client.sock = clientSock;

        client.address = inet_ntoa(clientAddr.sin_addr);
        client.port = ntohs(clientAddr.sin_port);
        client.arrivedAt = time(nullptr);

        queueMutex.lock();

        if (pendingClients.size() >= MAX_PENDING_CLIENTS){
        
            queueMutex.unlock();
            sendError(clientSock, "err, server busy, try again later");
            close(clientSock);
            continue;
        }

        pendingClients.enqueue(client);
        queueMutex.unlock();
    }

}

void dispatcherLoop(){
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.


    while (flag){
        ClientInfo client;
        bool hasClient = false;

        queueMutex.lock();

        if (!pendingClients.isEmpty()){
            client = pendingClients.dequeue();
            hasClient = true;
        }

        queueMutex.unlock();

        if (hasClient){
        
            if (time(nullptr) - client.arrivedAt > 30){
            
                sendError(client.sock, "err, client wait timeout");
                close(client.sock);
            }

            else{
                relayOneClient(client);
            }
        }

        else{
        
            usleep(100000);
        }
    }
}


void handler(int sg) {
    (void)sg;
    flag = 0;
}

int main(int argc, char* argv[]){

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().

    if (argc >= 2) {
        serverHost = argv[1];
    }

    if (argc >= 3) {
        serverPort = stoi(argv[2]);
    }

    if (argc >= 4) {
        lbPort = stoi(argv[3]);
    }

    int listenSock = createListenSocket(lbPort);

    thread acceptThread(acceptLoop, listenSock);
    acceptThread.detach();

    dispatcherLoop();

    close(listenSock);

    return 0;
}

    