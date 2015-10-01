#include "utp.h"

UTPSocket::UTPSocket(UTPContext *_utpctx, utp_socket *_sock, v8::Local<v8::Object>): utpctx(_utpctx), sock(_sock) {
	utp_set_userdata(sock, this);
	Wrap(sockObj);
	Ref();
}

NAN_METHOD(UTPSocket::Init) {
	Nan::HandleScope scope;
	v8::Isolate *isolate = info.GetIsolate();

	v8::Local<v8::FunctionTemplate> tpl = v8::FunctionTemplate::New(isolate, New);
	tpl->SetClassName(Nan::New("UTP"));
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	constructor.Reset(isolate, tpl->GetFunction());
}

NAN_METHOD(UTPSocket::New) {
	Nan::HandleScope scope;
	v8::Isolate *isolate = info.GetIsolate();

	info.GetReturnValue().Set(info.This());
}

UTPSocket *UTPSocket::create(UTPContext *utpctx, utp_socket *sock) {
	v8::Local<v8::Object> sockObj = UTPSocket::NewInstance(0);
	UTPSocket *utpsock(utpctx, sock, sockObj);
	return utpsock;
}

void UTPSocket::onConnect() {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onConnect"))).Call(0, 0);
	return 0;
}


void UTPSocket::onRead(const void *buf, size_t len) {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onRead"))).Call(1, { Nan::CopyBuffer(buf, len) });
	return 0;
}

void UTPSocket::onWritable() {
	writable = true;
	// try write
	return 0;
}

void UTPSocket::onEnd() {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onEnd"))).Call(0, 0);
	return 0;
}

void UTPSocket::onError(int errcode) {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onError"))).Call(0, 0);
	utp_close(sock);
	return 0;
}

void UTPSocket::onDestroy() {
	Nan::HandleScope scope;
	Nan::Callback(handle()->Get(Nan::New("_onDestroy"))).Call(0, 0);
	sock = nullptr;
	Unref();
	return 0;
}
