// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
#include <cerrno>
#include <sstream>
#include <arpa/inet.h>
#include <netinet/in.h>
using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char* TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};

struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int64_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char* buf, size_t n){
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.

    size_t totalReceived = 0;

    while (totalReceived < n){
        ssize_t received = recv( sock,buf + totalReceived,n - totalReceived, 0);

        if (received < 0){
        
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (received == 0) {
            return false;
        }

        totalReceived += received;
    }

    return true;
}

bool sendAll(int sock, const char* buf, size_t n){
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.

    size_t totalSent = 0;

    while (totalSent < n){ssize_t sent = send(sock,buf + totalSent,n - totalSent,MSG_NOSIGNAL);

        if (sent < 0){
        
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (sent == 0) {
            return false;
        }

        totalSent += sent;
    }

    return true;
}

int connectToHost(const char* host, int port){
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        return -1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &serverAddr.sin_addr) <= 0){
        close(sock);
        return -1;
    }

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0){
        close(sock);
        return -1;
    }

    return sock;
}

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

bool uploadSource(int sock, const char* sourcePath){
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.

    FILE* file = fopen(sourcePath, "rb");

    if (file == nullptr) {
        return false;
    }

    char status = 0;
    uint32_t crc = 0xFFFFFFFF;

    crc = calculateChecksum(crc, &status, 1);

    if (!sendAll(sock, &status, 1)){
        fclose(file);
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    size_t bytesRead;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0){
        crc = calculateChecksum(crc, buffer, bytesRead);

        if (!sendAll(sock, buffer, bytesRead)){
        
            fclose(file);
            return false;
        }
    }

    if (ferror(file)){
        fclose(file);
        return false;
    }

    fclose(file);

    crc ^= 0xFFFFFFFF;

    char checksum[4];

    for (int i = 0; i < 4; i++) {
        checksum[i] = (crc >> (8 * i)) & 0xFF;
    }

    if (!sendAll(sock, checksum, 4)) {
        return false;
    }
    shutdown(sock, SHUT_WR);
    return true;


}
bool receiveResponse(int sock, const char* outTdbgPath, string& outError){
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    TrailerChecker checker;
    char buffer[IO_BUFFER_SIZE];

    bool gotStatus = false;
    char status = -1;
    FILE* file = nullptr;

    while (true){
        ssize_t received = recv(sock, buffer, sizeof(buffer), 0);

        if (received < 0){
        
            if (errno == EINTR) {
                continue;
            }

            if (file != nullptr) {
                fclose(file);
            }
            remove(outTdbgPath);
            return false;
        }

        if (received == 0) {
            break;
        }

        string safeData = checker.addData(buffer, received);

        for (size_t i = 0; i < safeData.size(); i++){
            if (!gotStatus){
            
                status = safeData[i];
                gotStatus = true;

                if (status == 0){
                
                    file = fopen(outTdbgPath, "wb");

                    if (file == nullptr){
                    
                        outError = "err, could not create output file";
                        return false;
                    }
                }

                else if (status != 1){
                
                    outError = "err,invalid response status";
                    return false;
                }
            }
            else{
            
                if (status == 0){
                
                    fwrite(&safeData[i], 1, 1, file);
                }
                else{
                
                    outError += safeData[i];
                }
            }
        }
    }


    if (file != nullptr) {
        fclose(file);
    }
    if (!gotStatus || !checker.verify()){
    
        remove(outTdbgPath);
        outError = "rr, checksum mismatch, response corrupted";
        return false;
    }

    if (status == 1) {
        return false;
    }


    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char* path, TdbgReader& r)
{

   
}
void closeTdbg(TdbgReader& r)
{
    // Free r.index and close the file.

    
}


// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================


void closeSession(DebugSession& s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.

 
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.

}

bool cmdStart(DebugSession& s, const string& sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).


}


// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    
}
void printHelp()
{
    // Print the commands
    

}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.

  
}
int main(int argc, char* argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

  

}