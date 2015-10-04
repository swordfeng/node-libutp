#include "utp.h"

namespace nodeUTP {

unordered_set<utp_socket *> UTPSocket::activeSockets;
Nan::Persistent<v8::Function> UTPSocket::constructor;

UTPSocket::UTPSocket(UTPContext *_utpctx, utp_socket *_sock):
utpctx(_utpctx),
sock(_sock),
chunkLength(0),
chunkOffset(0),
connected(false),
paused(false),
readBuf(nullptr),
readLen(0),
refSelf(false)
{
	utp_set_userdata(sock, this);
	activeSockets.insert(sock);
	Nan::HandleScope scope;
	v8::Local<v8::Object> sockObj = Nan::New(constructor)->NewInstance(0, 0);
	Wrap(sockObj);
	uvRef();
	Ref();
}

UTPSocket::~UTPSocket() {
}

void UTPSocket::uvRef() {
	if (refSelf) return;
	refSelf = true;
	utpctx->sockRef();
}

void UTPSocket::uvUnref() {
	if (!refSelf) return;
	refSelf = false;
	utpctx->sockUnref();
}

void UTPSocket::Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module) {
	Nan::HandleScope scope;

	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
	tpl->SetClassName(Nan::New("UTP").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	Nan::SetPrototypeMethod(tpl, "write", Write);
	Nan::SetPrototypeMethod(tpl, "close", Close);
	Nan::SetPrototypeMethod(tpl, "forceTimedOut", ForceTimedOut);
	Nan::SetPrototypeMethod(tpl, "remoteAddress", RemoteAddress);
	Nan::SetPrototypeMethod(tpl, "slow", SlowSpeed);
	Nan::SetPrototypeMethod(tpl, "normal", NormalSpeed);
	Nan::SetPrototypeMethod(tpl, "ref", jsRef);
	Nan::SetPrototypeMethod(tpl, "unref", jsUnref);

	// do not expose constructor
	//exports->Set(Nan::New("UTPSocket").ToLocalChecked(), tpl->GetFunction());
	constructor.Reset(tpl->GetFunction());
}

NAN_METHOD(UTPSocket::New) {
	Nan::HandleScope scope;
	info.GetReturnValue().Set(info.This());
}

NAN_METHOD(UTPSocket::Write) {
	Nan::HandleScope scope;
	v8::Local<v8::Object> buf = info[0].As<v8::Object>();
	v8::Local<v8::Function> cb = info[1].As<v8::Function>();
	UTPSocket *utpsock = get(info.Holder());
	const char *chunk = node::Buffer::Data(buf);
	size_t len = node::Buffer::Length(buf);
	unique_ptr<char[]> newchunk(new char[len]);
	std::copy(chunk, chunk + len, newchunk.get());
	utpsock->setChunk(std::move(newchunk), len, cb);
	utpsock->write();
}

NAN_METHOD(UTPSocket::Close) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	assert(utpsock->sock);
	utpsock->onEnd();
}

NAN_METHOD(UTPSocket::ForceTimedOut) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utpsock->onError(UTP_ETIMEDOUT);
}

NAN_METHOD(UTPSocket::SlowSpeed) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utp_setsockopt(utpsock->sock, UTP_RCVBUF, 4096);
}

NAN_METHOD(UTPSocket::NormalSpeed) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utp_setsockopt(utpsock->sock, UTP_RCVBUF, 1048576);
}

NAN_METHOD(UTPSocket::RemoteAddress) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = Nan::ObjectWrap::Unwrap<UTPSocket>(info.Holder());
	if (activeSockets.find(utpsock->sock) == activeSockets.end()) {
		info.GetReturnValue().Set(Nan::Null());
	} else {
		union {
			struct sockaddr saddr;
			struct sockaddr_in sin;
			struct sockaddr_in6 sin6;
		} addr;
		socklen_t len = sizeof(addr);
		assert(utp_getpeername(utpsock->sock, &addr.saddr, &len) >= 0);
		char address[50];
		v8::Local<v8::Object> res = Nan::New<v8::Object>();
		assert(addr.saddr.sa_family == AF_INET || addr.saddr.sa_family == AF_INET6);
		if (addr.saddr.sa_family == AF_INET) {
			// ipv4
			assert(uv_ip4_name(&addr.sin, address, 50) >= 0);
			res->Set(Nan::New("address").ToLocalChecked(), Nan::New(address).ToLocalChecked());
			res->Set(Nan::New("family").ToLocalChecked(), Nan::New("IPv4").ToLocalChecked());
			res->Set(Nan::New("port").ToLocalChecked(), Nan::New<v8::Uint32>(ntohs(addr.sin.sin_port)));
		} else {
			// ipv6
			assert(uv_ip6_name(&addr.sin6, address, 50) >= 0);
			res->Set(Nan::New("address").ToLocalChecked(), Nan::New(address).ToLocalChecked());
			res->Set(Nan::New("family").ToLocalChecked(), Nan::New("IPv6").ToLocalChecked());
			res->Set(Nan::New("port").ToLocalChecked(), Nan::New<v8::Uint32>(ntohs(addr.sin6.sin6_port)));
		}
		info.GetReturnValue().Set(res);
	}

}

NAN_METHOD(UTPSocket::jsRef) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utpsock->uvRef();
}

NAN_METHOD(UTPSocket::jsUnref) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utpsock->uvUnref();
}

NAN_METHOD(UTPSocket::CleanUp) {
	for (auto sock: activeSockets) {
		utp_close(sock);
	}
	activeSockets.empty();
}

void UTPSocket::setChunk(unique_ptr<char[]>&& _chunk, size_t len, v8::Local<v8::Function> cb) {
	assert(!chunk.get());
	chunk = std::move(_chunk);
	chunkLength = len;
	chunkOffset = 0;
	writeCb.SetFunction(cb);
}

void UTPSocket::write() {
	char *data = chunk.get();
	if (!sock || !connected || !data) return;
	while (chunkOffset < chunkLength || chunkLength == 0) {
		size_t len = chunkLength - chunkOffset;
		size_t sent = utp_write(sock, data + chunkOffset, len);
		chunkOffset += sent;
		if (sent == 0) break;
	}
	if (chunkOffset == chunkLength) {
		Nan::HandleScope scope;
		chunkOffset = chunkLength = 0;
		chunk.reset(nullptr);
		writeCb.Call(0, 0);
	}
}

void UTPSocket::onConnect() {
	Nan::HandleScope scope;
	connected = true;
	Nan::Callback(handle()->Get(Nan::New("_onConnect").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
	write();
}

void UTPSocket::onRead(const void *_buf, size_t len) {
	Nan::HandleScope scope;
	utp_read_drained(sock);
	v8::Local<v8::Value> argv[] = { Nan::CopyBuffer(static_cast<const char *>(_buf), len).ToLocalChecked() };
	Nan::Callback(handle()->Get(Nan::New("_onRead").ToLocalChecked()).As<v8::Function>()).Call(1, argv);
}

void UTPSocket::onWritable() {
	write();
}

void UTPSocket::onEnd() {
	if (activeSockets.find(sock) != activeSockets.end()) {
		utp_close(sock);
		activeSockets.erase(sock);
	}
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onEnd").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
}

void UTPSocket::onError(int errcode) {
	Nan::HandleScope scope;
	const char *errstr = "unknown error", *errname = "UNKNOWN";
	switch (errcode) {
	case UTP_ECONNREFUSED:
		errstr = "connection refused";
		errname = "ECONNREFUSED";
		break;
	case UTP_ECONNRESET:
		errstr = "connection reset by peer";
		errname = "ECONNRESET";
		break;
	case UTP_ETIMEDOUT:
		errstr = "connection timed out";
		errname = "ETIMEDOUT";
		break;
	}
	v8::Local<v8::Value> err = Nan::Error(errstr);
	Nan::To<v8::Object>(err).ToLocalChecked()->Set(Nan::New("code").ToLocalChecked(), Nan::New(errname).ToLocalChecked());
	v8::Local<v8::Value> argv[] = {err};
	Nan::Callback(handle()->Get(Nan::New("_onError").ToLocalChecked()).As<v8::Function>()).Call(1, argv);
	if (activeSockets.find(sock) != activeSockets.end()) {
		utp_close(sock);
		activeSockets.erase(sock);
	}
}

void UTPSocket::onDestroy() {
	if (!sock) return;
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onDestroy").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
	sock = nullptr;
	uvUnref();
	Unref();
	MakeWeak();
}


}
