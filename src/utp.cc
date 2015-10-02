#include "utp.h"

namespace nodeUTP {

void InitAll(v8::Local<v8::Object> exports, v8::Local<v8::Object> module) {
    Nan::HandleScope scope;

    UTPContext::Init(exports, module);
    UTPSocket::Init(exports, module);
}

}

NODE_MODULE(utp, nodeUTP::InitAll)
