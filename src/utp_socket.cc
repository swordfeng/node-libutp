#include "utp.h"

namespace nodeUTP {

Nan::Persistent<v8::Function> UTPSocket::constructor;

UTPSocket::UTPSocket(UTPContext *_utpctx, utp_socket *_sock):
utpctx(_utpctx),
sock(_sock),
chunkLength(0),
chunkOffset(0),
connected(false),
paused(false),
readBuf(nullptr),
readLen(0)
{
	utp_set_userdata(sock, this);
	Nan::HandleScope scope;
	v8::Local<v8::Object> sockObj = Nan::New(constructor)->NewInstance(0, 0);
	Wrap(sockObj);
	Ref();
}

UTPSocket::~UTPSocket() {
}

void UTPSocket::Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module) {
	Nan::HandleScope scope;

	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
	tpl->SetClassName(Nan::New("UTP").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	Nan::SetPrototypeMethod(tpl, "write", Write);
	Nan::SetPrototypeMethod(tpl, "close", Close);
	Nan::SetPrototypeMethod(tpl, "pause", Pause);
	Nan::SetPrototypeMethod(tpl, "resume", Resume);

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
	utpsock->setChunk(node::Buffer::Data(buf), node::Buffer::Length(buf), cb);
	utpsock->write();
}

NAN_METHOD(UTPSocket::Close) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	assert(utpsock->sock);
	utpsock->onEnd();
}

NAN_METHOD(UTPSocket::Pause) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utpsock->paused = true;
}

NAN_METHOD(UTPSocket::Resume) {
	Nan::HandleScope scope;
	UTPSocket *utpsock = get(info.Holder());
	utpsock->paused = false;
	utpsock->read();
}

void UTPSocket::setChunk(const char *_chunk, size_t len, v8::Local<v8::Function> cb) {
	assert(!chunk.get());
	chunk.reset(new char[len]);
	std::copy(_chunk, _chunk + len, chunk.get());
	chunkLength = len;
	chunkOffset = 0;
	writeCb.SetFunction(cb);
}

void UTPSocket::write() {
	char *data = chunk.get();
	if (!sock || !connected || !data) return;
	int t = 5;
	while (chunkOffset < chunkLength || chunkLength == 0) {
		size_t len = chunkLength - chunkOffset;
		size_t sent = utp_write(sock, data + chunkOffset, len);
		chunkOffset += sent;
		if (sent == 0) break;
	}
	if (chunkOffset == chunkLength) {
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

void UTPSocket::read() {
	if (!sock || paused || !readBuf) return;
	v8::Local<v8::Value> argv[] = { Nan::CopyBuffer(readBuf, readLen).ToLocalChecked() };
	Nan::Callback(handle()->Get(Nan::New("_onRead").ToLocalChecked()).As<v8::Function>()).Call(1, argv);
	utp_read_drained(sock);
}

void UTPSocket::onRead(const void *buf, size_t len) {
	Nan::HandleScope scope;
	readBuf = static_cast<const char *>(buf);
	readLen = len;
	read();
}

void UTPSocket::onWritable() {
	write();
}

void UTPSocket::onEnd() {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onEnd").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
	utp_close(sock);
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
	utp_close(sock);
}

void UTPSocket::onDestroy() {
	if (!sock) return;
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onDestroy").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
	sock = nullptr;
	Unref();
	MakeWeak();
}


}
