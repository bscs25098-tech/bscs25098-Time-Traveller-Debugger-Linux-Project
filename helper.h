#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
using namespace std;

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

class TrailerChecker{
private:

    uint32_t crc = 0xFFFFFFFF;
    unsigned char trailer[4] = {};
    size_t trailerCount = 0;


    void updateByte(unsigned char byte){
        crc ^= byte;

        for (int i = 0; i < 8; i++){
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            }

            else {
                crc >>= 1;
            }
        }
    }


public:

    string addData(const char* data, size_t length){
        string safeData;


        for (size_t i = 0; i < length; i++){
        
            if (trailerCount < 4){
            
                trailer[trailerCount] = (unsigned char)data[i];
                trailerCount++;
            }

            else{
            
                safeData += (char)trailer[0];
                updateByte(trailer[0]);
                trailer[0] = trailer[1];
                trailer[1] = trailer[2];
                trailer[2] = trailer[3];
                trailer[3] = (unsigned char)data[i];
            }
        }

        return safeData;
    }

    bool verify(){

        if (trailerCount != 4) {
            return false;
        }

        uint32_t calculated = crc ^ 0xFFFFFFFF;
        uint32_t received = (uint32_t)trailer[0] | ((uint32_t)trailer[1] << 8) | ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
        return calculated == received;

    }


    string getTrailer(){
        string r;

        for (int i = 0; i < 4; i++){
        
            r += (char)trailer[i];
        }


        return r;
    }
};