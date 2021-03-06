extern "C" {
#include <utp.h>
#ifndef __SSIZE_T__
#define __SSIZE_T__ // dulplicate definition hack
#define DEF__SSIZE_T__
#endif
#include <uv.h>
#ifdef DEF__SSIZE_T__
#undef __SSIZE_T__ // dulplicate definition hack
#undef DEF__SSIZE_T__
#endif
}
#include <node.h>
#include <node_object_wrap.h>
#include <nan.h>
#include <unordered_set>
#include <cassert>
#include <cstdio>
#include <memory>
#include <functional>
#include <algorithm>
#include <iostream>
#include <new>

namespace nodeUTP {

using std::string;
using std::unique_ptr;
using std::function;
using std::vector;
using std::nothrow;
using std::unordered_set;

class UTPContext;
class UTPSocket;

class UTPContext final : public Nan::ObjectWrap {
public:
    enum {
        STATE_INIT = 0,
        STATE_BOUND,
        STATE_STOPPED
    };
    static const char *statestr[];
private:
	static Nan::Persistent<v8::Function> constructor;

	uv_udp_t udpHandle;
	uv_timer_t timerHandle;
	unique_ptr<utp_context, function<void (utp_context *)>> ctx;
	int state;
	bool listening;
	int backlog;
	int connections;
	int pendingConnections;
    int refCount;
    bool refSelf;

	void uvRecv(ssize_t len, const void *buf, const struct sockaddr *addr, unsigned flags);
	uint64 sendTo(const void *buf, size_t len, const struct sockaddr *addr, socklen_t addrlen);
	bool onFirewall();
	void onAccept(utp_socket *sock);

	uint64 onCallback(utp_callback_arguments *a);

    int bind(uint16_t port, string host);
    void listen(int _backlog);
    int connect(uint16_t port, string host, UTPSocket **putpsock);
    void stop();
    void destroy();

    void uvRef();
    void uvUnref();

	static NAN_METHOD(New);
	static NAN_METHOD(Bind);
	static NAN_METHOD(Listen);
	static NAN_METHOD(Connect);
	static NAN_METHOD(Close);
    static NAN_METHOD(State);
    static NAN_METHOD(Address);
	static NAN_METHOD(jsRef);
	static NAN_METHOD(jsUnref);

public:
	static void Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module);
	UTPContext();
	~UTPContext();
    void sockRef();
    void sockUnref();
};

class UTPSocket final : public Nan::ObjectWrap {
private:
    static unordered_set<utp_socket *> activeSockets;
	static Nan::Persistent<v8::Function> constructor;

	UTPContext *const utpctx;
	utp_socket *sock;
	unique_ptr<char[]> chunk;
    size_t chunkLength;
	size_t chunkOffset;
    Nan::Callback writeCb;
	bool connected;

    bool paused;
    unique_ptr<char[]> readBuf;
    size_t readLen;

    bool refSelf;

    void setChunk(unique_ptr<char[]>&& _chunk, size_t len, v8::Local<v8::Function> cb);
    void write();

    void uvRef();
    void uvUnref();

	static NAN_METHOD(New);
	static NAN_METHOD(Write);
	static NAN_METHOD(Close);
	static NAN_METHOD(ForceTimedOut);
	static NAN_METHOD(SlowSpeed);
	static NAN_METHOD(NormalSpeed);
	static NAN_METHOD(RemoteAddress);
	static NAN_METHOD(jsRef);
	static NAN_METHOD(jsUnref);
public:
    static void Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module);
	static UTPSocket *get(utp_socket *sock) {
		return static_cast<UTPSocket *>(utp_get_userdata(sock));
	}
	static UTPSocket *get(v8::Local<v8::Object> obj) {
		return ObjectWrap::Unwrap<UTPSocket>(obj);
	}
    UTPSocket(UTPContext *_utpctx, utp_socket *_sock);
    ~UTPSocket();
	void onRead(const void *buf, size_t len);
	void onError(int errcode);
    void onConnect();
    void onWritable();
    void onEnd();
    void onDestroy();

    static NAN_METHOD(CleanUp);
};
}
