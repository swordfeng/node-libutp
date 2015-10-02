#include "utp.h"

UTPSocket::UTPSocket(UTPContext *_utpctx, utp_socket *_sock, v8::Local<v8::Object> sockObj): utpctx(_utpctx), sock(_sock) {
	utp_set_userdata(sock, this);
	Wrap(sockObj);
	Ref();
}

NAN_METHOD(UTPSocket::Init) {
	Nan::HandleScope scope;

	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
	tpl->SetClassName(Nan::New("UTP").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

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
	utp_close(utpsock->sock);
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
	char *buf = new char[len];
	std::copy(_chunk, _chunk + len, buf);
	assert(!chunk.get());
	chunk.reset(buf);
	chunkLength = len;
	chunkOffset = 0;
	writeCb.SetFunction(cb);
}

void UTPSocket::write() {
	char *data = chunk.get();
	if (!data) return;
	while (chunkOffset < chunkLength) {
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

UTPSocket *UTPSocket::create(UTPContext *utpctx, utp_socket *sock) {
	Nan::HandleScope scope;
	v8::Local<v8::Object> sockObj = Nan::New(constructor)->NewInstance(0, 0);
	return new UTPSocket(utpctx, sock, sockObj);
}

void UTPSocket::onConnect() {
	Nan::HandleScope scope;
	writable = true;
	Nan::Callback(handle()->Get(Nan::New("_onConnect").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
}

void UTPSocket::read() {
	if (paused || !readBuf) return;
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
	writable = true;
	write();
}

void UTPSocket::onEnd() {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onEnd").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
}

void UTPSocket::onError(int errcode) {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onError").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
	utp_close(sock);
}

void UTPSocket::onDestroy() {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onDestroy").ToLocalChecked()).As<v8::Function>()).Call(0, 0);
	sock = nullptr;
	Unref();
}
