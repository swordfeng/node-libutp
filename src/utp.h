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
extern "C" {
#include <netinet/in.h>
}



using std::string;
using std::unique_ptr;
using std::function;
using std::vector;

class UTPContext final : public Nan::ObjectWrap {
public:
    enum {
        STATE_INIT = 0,
        STATE_BOUND,
        STATE_STOPPED
    };
private:
	static Nan::Persistent<v8::Function> constructor;

	uv_udp_t udpHandle;
	uv_timer_t timerHandle;
	unique_ptr<utp_context, function<void (utp_context *)>> ctx;
	int state = STATE_INIT;
	bool listening = false;
	int backlog = 0;
	int connections = 0;

	void uvRecv(ssize_t len, const void *buf, const struct sockaddr *addr, unsigned flags);
	uint64 sendTo(const void *buf, size_t len, const struct sockaddr *addr, socklen_t addrlen);
	bool onFirewall();
	void onAccept(utp_socket *sock);

	uint64 onCallback(utp_callback_arguments *a);

	static NAN_METHOD(Init);
	static NAN_METHOD(New);
	static NAN_METHOD(Bind);
	static NAN_METHOD(Listen);
	static NAN_METHOD(Connect);
	static NAN_METHOD(Close);
	//static NAN_METHOD(uvRef);
	//static NAN_METHOD(uvUnref);
public:
	UTPContext();
	~UTPContext();
	int bind(uint16_t port, string host);
	void listen(int _backlog);
	utp_socket *connect(uint16_t port, string host);
    void stop();
	void close();
};

class UTPSocket final : public Nan::ObjectWrap {
private:
	static Nan::Persistent<v8::Function> constructor;

	const UTPContext *utpctx;
	utp_socket *sock;
	unique_ptr<char[]> chunk;
    size_t chunkLength = 0;
	size_t chunkOffset = 0;
    Nan::Callback writeCb;
	bool writable = false;

    bool paused = false;
    const char *readBuf = nullptr;
    size_t readLen = 0;

	UTPSocket(UTPContext *_utpctx, utp_socket *_sock, v8::Local<v8::Object> sockObj);
    void setChunk(const char *_chunk, size_t len, v8::Local<v8::Function> cb);
    void write();
    void read();

	static NAN_METHOD(Init);
	static NAN_METHOD(New);
	static NAN_METHOD(Write);
	static NAN_METHOD(Close);
	static NAN_METHOD(Pause);
	static NAN_METHOD(Resume);
public:
	~UTPSocket() {};
	static UTPSocket *get(utp_socket *sock) {
		return static_cast<UTPSocket *>(utp_get_userdata(sock));
	}
	static UTPSocket *get(v8::Local<v8::Object> obj) {
		return ObjectWrap::Unwrap<UTPSocket>(obj);
	}
	static UTPSocket *create(UTPContext *utpctx, utp_socket *sock);
	void onRead(const void *buf, size_t len);
	void onError(int errcode);
    void onConnect();
    void onWritable();
    void onEnd();
    void onDestroy();
};
