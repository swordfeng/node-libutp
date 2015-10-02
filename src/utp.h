#include <uv.h>
#include <utp.h>
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
extern "C" {
#include <netinet/in.h>
}

namespace nodeUTP {

using std::string;
using std::unique_ptr;
using std::function;
using std::vector;
using std::nothrow;

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

	void uvRecv(ssize_t len, const void *buf, const struct sockaddr *addr, unsigned flags) noexcept;
	uint64 sendTo(const void *buf, size_t len, const struct sockaddr *addr, socklen_t addrlen) noexcept;
	bool onFirewall() noexcept;
	void onAccept(utp_socket *sock) noexcept;

	uint64 onCallback(utp_callback_arguments *a) noexcept;

    int bind(uint16_t port, string host) noexcept;
    void listen(int _backlog) noexcept;
    int connect(uint16_t port, string host, UTPSocket **putpsock) noexcept;
    void stop() noexcept;
    void destroy() noexcept;

	static NAN_METHOD(New);
	static NAN_METHOD(Bind);
	static NAN_METHOD(Listen);
	static NAN_METHOD(Connect);
	static NAN_METHOD(Close);
    static NAN_METHOD(State);
	//static NAN_METHOD(uvRef);
	//static NAN_METHOD(uvUnref);

public:
	static void Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module);
	UTPContext() noexcept;
	~UTPContext() noexcept;
};

class UTPSocket final : public Nan::ObjectWrap {
private:
	static Nan::Persistent<v8::Function> constructor;

	const UTPContext *utpctx;
	utp_socket *sock;
	unique_ptr<char[]> chunk;
    size_t chunkLength;
	size_t chunkOffset;
    Nan::Callback writeCb;
	bool connected;

    bool paused;
    const char *readBuf;
    size_t readLen;

    void setChunk(const char *_chunk, size_t len, v8::Local<v8::Function> cb) noexcept;
    void write() noexcept;
    void read() noexcept;

	static NAN_METHOD(New);
	static NAN_METHOD(Write);
	static NAN_METHOD(Close);
	static NAN_METHOD(Pause);
	static NAN_METHOD(Resume);
public:
    static void Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module);
	static UTPSocket *get(utp_socket *sock) noexcept {
		return static_cast<UTPSocket *>(utp_get_userdata(sock));
	}
	static UTPSocket *get(v8::Local<v8::Object> obj) noexcept {
		return ObjectWrap::Unwrap<UTPSocket>(obj);
	}
    UTPSocket(UTPContext *_utpctx, utp_socket *_sock) noexcept;
    ~UTPSocket() noexcept;
	void onRead(const void *buf, size_t len) noexcept;
	void onError(int errcode) noexcept;
    void onConnect() noexcept;
    void onWritable() noexcept;
    void onEnd() noexcept;
    void onDestroy() noexcept;
};
}
