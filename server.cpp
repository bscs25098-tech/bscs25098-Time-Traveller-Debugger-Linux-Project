// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack{

    struct Node{
        T data;
        Node* next;

    };

    Node* top;
    int32_t count;

public:

    Stack(){
        top = nullptr;
        count = 0;
    }

    void push(const T& val){
        Node* n = new Node;
        n->data = val;
        n->next = top;
        top = n;
        count++;
      
    }

    T pop(){

        Node* temp = top;
        T value = temp ->data;
        top = top->next;
        delete temp;
        count--;

        return value;
    
    }

    T& peek(){

        return top->data;
    }

    bool isEmpty(){
        
        return count == 0;

    }

    int32_t depth(){
      
        return count;
    }

    int32_t snapshot_into(T out[], int32_t maxLen){

        Node* temp = top;
        int ct = 0;

        while (temp != nullptr && ct < maxLen) {
            out[ct] = temp->data;
            ct++;
            temp = temp->next;


        }
        return ct;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode{

    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};


class Timeline{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
  
    Timeline(){
        stepCount = 0;
        head = nullptr;
        tail = nullptr;
    }

    void record(Snapshot* s){

        TimelineNode* n= new TimelineNode;

        n->data = s;
        n->next = nullptr;
        n->prev = tail;

        if (tail == nullptr){
            head = n;
            tail = n;
        }

        else{
            tail->next = n;
            tail = n;
        }

        stepCount++;
    }

    TimelineNode* begin(){

        return head;
    }

    int32_t getStepCount(){

        return stepCount;

    }
};

// Core structs
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

void writeHeader(FILE* f, const TTDBHeader& h){
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry{

    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};

struct PendingPatch{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out){
   
    while (getline(in, out)) {

        if (!out.empty()) {
            return true;
        }
    }
    return false;

}

string firstWord(const string& line){
    int i = 0;
    string word = "";

    while (i < line.length() && line[i] != ' ') {
        word += line[i];
        i++;

    }
    return word;
}

string secondWord(const string& line){
    int i = 0;
    string word = "";
    int spaces = 0;

    while (i < line.length()){
        if (line[i] == ' ') {

            spaces++;
        }

        else if (spaces == 1) {

            word += line[i];
        }
        else if (spaces > 1){
            break;
        }

        i++;
    }

    return word;
}

bool validateProgram(const char* sourcePath){
    ifstream in(sourcePath);
    if (!in){
        return false;
    }

    string line;
    bool inside_func = false;

    while (readSourceLine(in, line)){
        string word = firstWord(line);

        if (word == "func"){
            if (inside_func){

                return false;
            }

            inside_func = true;
        }

        else if (word == "func_end"){
            if (!inside_func){
            
                return false;
            }

            inside_func = false;
        }
    }

    if (inside_func){
        return false;
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text){
    int32_t size = text.length();
    
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), sizeof(char), size, f);

    return offsetField;
    
}

int64_t readResolveRecord(FILE* f, string& outText){
    int32_t size;
    int64_t offset;

    if (fread(&offset, sizeof(int64_t), 1, f) != 1){
        return -1;
    }

    if (fread(&size, sizeof(int32_t), 1, f) != 1){
    
        return -1;
    }
    outText = "";

    for (int i = 0; i < size; i++){
        char ch;

        if (fread(&ch, sizeof(char), 1, f) != 1){
            return -1;
        }

        outText += ch;
    }

    return offset;
}


int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath){
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream source(sourcePath);
    FILE* resolveFile = fopen(resolveBinPath, "wb+");
   
    if (!source || resolveFile == nullptr) {
        if (resolveFile != nullptr) {
            fclose(resolveFile);
        }
        return -1;
    }

    string line;
    int64_t current_offset = 0;
    while (readSourceLine(source, line)) {
        string w1 = firstWord(line);
        string w2 = secondWord(line);


        writeResolveRecord(resolveFile, current_offset, line);


        if (w1 == "func") {
            funcArray[funcCount].funcName = w2;
            funcArray[funcCount].byteOffsetInResolveBin = current_offset;
            funcCount++;
        }

        if (w1 == "call") {
            patches[patchCount].byteOffsetOfOffsetField = current_offset;
            patches[patchCount].targetFuncName = w2;
            patchCount++;
        }

        current_offset = current_offset + 8 + 4 + line.length();
    }

        int64_t main_offset = -1;
        for (int i = 0; i < funcCount; i++) {
            if (funcArray[i].funcName == "main") {
                main_offset = funcArray[i].byteOffsetInResolveBin;
                break;
            }
        }

        if (main_offset == -1) {
            fclose(resolveFile);
            return -1;
        }

        for (int i = 0; i < patchCount; i++) {
            int64_t target_offset = -1;
            for (int j = 0; j < funcCount; j++) {

                if (funcArray[j].funcName == patches[i].targetFuncName) {
                    target_offset = funcArray[j].byteOffsetInResolveBin;
                    break;
                }
            }

            if (target_offset == -1) {
                fclose(resolveFile);
                return -1;
            }

            fseek(resolveFile, patches[i].byteOffsetOfOffsetField, SEEK_SET);
            fwrite(&target_offset, sizeof(int64_t), 1, resolveFile);
        }

        fclose(resolveFile);
        return main_offset;

    
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType{
    KEYWORD,
    IDENTIFIER,
    PARAM

};

struct Token{

    TokenType type;
    string text;
};

int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens){
    int ct = 0;
    int i = 0;

    while (i < line.length() && ct < maxTokens){
       
        while (i < line.length() && line[i] == ' '){     
            i++;
        }

        if (i >= line.length()){
            break;
        }

        string word = "";
        while (i < line.length() && line[i] != ' '){
            word += line[i];
            i++;
        }

        if (ct == 0){
            tokens[ct].type = KEYWORD;
        }

        else if (ct == 1){
            tokens[ct].type = IDENTIFIER;
        }

        else{
        
            tokens[ct].type = PARAM;
        }
        tokens[ct].text = word;
        ct++;
    }

    return ct;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack){

    Snapshot* s = new Snapshot;
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);

    return s;

}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline) {

    FILE* file = fopen(resolveBinPath, "rb");

    if (file == nullptr) {
        return;
    }

    Stack<Frame> callStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);
    fseek(file, mainOffset, SEEK_SET);

    string line;
    bool endProgram = false;

    while (true) {
        int64_t offset = readResolveRecord(file, line);


        if (offset == -1) {
            break;
        }

        int64_t nextPosition = ftell(file);
        Token tokens[MAX_TOKENS];
        int32_t token_ct = tokenizeLine(line, tokens, MAX_TOKENS);

        if (token_ct == 0) {
            continue;
        }

        if ((tokens[0].text == "set" ||tokens[0].text == "add" ||tokens[0].text == "sub" ||tokens[0].text == "mul" ||tokens[0].text == "div") && token_ct < 3 || tokens[0].text == "call" && token_ct < 2){
            break;
        }

        Frame& current_frame = callStack.peek();
        if (tokens[0].text == "set"){
          
            if (current_frame.localCount >= MAX_VARS_PER_FRAME){
                break;
            }

            int value = stoi(tokens[2].text);
            current_frame.locals[current_frame.localCount].name = tokens[1].text;
            current_frame.locals[current_frame.localCount].value = value;
            current_frame.localCount++;
        }

        else if (tokens[0].text == "add"){
            for (int i = 0; i < current_frame.localCount; i++){
            
                if (current_frame.locals[i].name == tokens[1].text){
                    current_frame.locals[i].value = current_frame.locals[i].value + stoi(tokens[2].text);
                    break;
                }
            }
        }

        else if (tokens[0].text == "sub"){
            for (int i = 0; i < current_frame.localCount; i++){
            
                if (current_frame.locals[i].name == tokens[1].text){
                    current_frame.locals[i].value =current_frame.locals[i].value - stoi(tokens[2].text);
                    break;
                }
            }
        }

        else if (tokens[0].text == "mul"){
            for (int i = 0; i < current_frame.localCount; i++){
            
                if (current_frame.locals[i].name == tokens[1].text){
                    current_frame.locals[i].value = current_frame.locals[i].value * stoi(tokens[2].text);      
                    break;
                }
            }
        }

        else if (tokens[0].text == "div"){
            for (int i = 0; i < current_frame.localCount; i++){
                
                if (current_frame.locals[i].name == tokens[1].text){
                    current_frame.locals[i].value = current_frame.locals[i].value / stoi(tokens[2].text);
                    break;
                }
            }
        }

        else if (tokens[0].text == "call"){
       
            if (callStack.depth() >= MAX_STACK_DEPTH){
                break;
            }

            Frame new_frame;
            new_frame.func_name = tokens[1].text;
            new_frame.argc = 0;
            new_frame.returnLine = nextPosition;
            new_frame.localCount = 0;
            callStack.push(new_frame);

            fseek(file, offset, SEEK_SET);
        }


        else if (tokens[0].text == "func_end"){
            if (callStack.depth() == 1){
                endProgram = true;
            }

            else{
            
                int64_t returnPosition = callStack.peek().returnLine;
                callStack.pop();
                fseek(file, returnPosition, SEEK_SET);
            }
        }


        Snapshot* snapshot = buildSnapshot(callStack);
        timeline.record(snapshot);
        
        if (endProgram){
            break;
        }

    }
    fclose(file);

}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath){

    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}