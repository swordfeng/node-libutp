#include "utp.h"

UTPContext::UTPContext(): ctx(utp_init(2), utp_destroy) {
	assert(uv_udp_init(uv_default_loop(), &udpHandle) >= 0);
	assert(uv_timer_init(uv_default_loop(), &timerHandle) >= 0);

	utp_context_set_userdata(ctx.get(), this);
	udpHandle.data = this;
	timerHandle.data = this;

	for (int type: vector<int>({UTP_SENDTO, UTP_ON_ERROR, UTP_ON_STATE_CHANGE, UTP_ON_READ, UTP_ON_FIREWALL, UTP_ON_ACCEPT})) {
		utp_set_callback(ctx.get(), type, [] (utp_callback_arguments *a) {
			UTPContext *utpctx = static_cast<UTPContext *>(utp_context_get_userdata(a->context));
			return utpctx->onCallback(a);
		});
	}
}

UTPContext::~UTPContext() {
	if (state == STATE_BOUND || state == STATE_STOPPED) {
		assert(uv_timer_stop(&timerHandle) >= 0);
		assert(uv_udp_recv_stop(&udpHandle) >= 0);
	}
	uv_close(reinterpret_cast<uv_handle_t *>(&udpHandle), nullptr);
	uv_close(reinterpret_cast<uv_handle_t *>(&timerHandle), nullptr);
}

int UTPContext::bind(uint16_t port, string host) {
	if (state != STATE_INIT) return 0;
	union {
		struct sockaddr saddr;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
	} addr;
	int errcode = uv_ip4_addr(host.c_str(), port, &addr.sin);
	if (errcode < 0) errcode = uv_ip6_addr(host.c_str(), port, &addr.sin6);
	if (errcode < 0) return errcode;
	errcode = uv_udp_bind(&udpHandle, &addr.saddr, UV_UDP_REUSEADDR);
	if (errcode < 0) return errcode;
	assert(uv_udp_recv_start(&udpHandle, static_cast<void (*)(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)> (
		[] (uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
		    buf->base = static_cast<char *>(malloc(suggested_size));
		    buf->len = suggested_size;
		}
	), [] (uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
			static_cast<UTPContext *>(handle->data)->uvRecv(nread, buf->base, addr, flags);
			free(buf->base);
		}
	) == 0);
	assert(uv_timer_start(&timerHandle, static_cast<void (*)(uv_timer_t *handle)> ([] (uv_timer_t *handle) -> void {
		UTPContext *ctx = static_cast<UTPContext *>(handle->data);
		utp_check_timeouts(ctx->ctx.get());
	}), 0, 500) >= 0);
	Ref();
	state = STATE_BOUND;
	return 0;
};


UTPSocket *UTPContext::connect(uint16_t port, string host) {
	if (state != STATE_BOUND) return nullptr;
	return UTPSocket::create(this, utp_create_socket(ctx.get()));
}

void UTPContext::listen(int _backlog) {
	if (state != STATE_BOUND) return;
	backlog = _backlog;
	if (backlog < 0) backlog = 0;
	listening = true;
	return;
}

void UTPContext::stop() {
	listening = false;
	state = STATE_STOPPED;
	if (connections == 0 && state == STATE_STOPPED) Unref();
	return;
}

uint64 UTPContext::onCallback(utp_callback_arguments *a) {
	UTPSocket *utpsock;
	switch (a->callback_type) {
	case UTP_SENDTO:
		return sendTo(a->buf, a->len, a->address, a->address_len);
	case UTP_ON_FIREWALL:
		return static_cast<uint64>(onFirewall());
	case UTP_ON_ACCEPT:
		connections++;
		onAccept(a->socket);
		return 0;

	case UTP_ON_READ:
		utpsock = UTPSocket::get(a->socket);
		UTPSocket::get(a->socket)->onRead(a->buf, a->len);
		return 0;
	case UTP_ON_STATE_CHANGE:
		utpsock = UTPSocket::get(a->socket);
		switch (a->state) {
		case UTP_STATE_CONNECT:
			utpsock->onConnect();
			return 0;
		case UTP_STATE_WRITABLE:
			utpsock->onWritable();
			return 0;
		case UTP_STATE_EOF:
			utpsock->onEnd();
			return 0;
		case UTP_STATE_DESTROYING:
			utpsock->onDestroy();
			connections--;
			if (connections == 0 && state == STATE_STOPPED) Unref();
			return 0;
		}
		return 0;
	case UTP_ON_ERROR:
		utpsock = UTPSocket::get(a->socket);
		utpsock->onError(a->error_code);
		return 0;
	}
	return 0;
}

void UTPContext::uvRecv(ssize_t len, const void *buf, const struct sockaddr *addr, unsigned flags) {
	assert(len >= 0);
	if (!len && !addr) {
		// no more data
	} else if (!utp_process_udp(ctx.get(), static_cast<const byte *>(buf), len, addr, addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(sockaddr_in6))) {
		printf("UDP packet not handled by UTP.  Ignoring.\n");
	}
	utp_check_timeouts(ctx.get());
}

uint64 UTPContext::sendTo(const void *buf, size_t len, const struct sockaddr *addr, socklen_t addrlen) {
	unique_ptr<char[]> tmpbuf(new char[len]);
	memcpy(tmpbuf.get(), buf, len);
	uv_buf_t uvbuf = {tmpbuf.get(), len};
	uv_udp_try_send(&udpHandle, &uvbuf, 1, addr);
	return 0;
}

bool UTPContext::onFirewall() {
	if (!listening) return true; // not a listen socket
	if (backlog > 0 && connections >= backlog) return true; // connections reach limit
	return false;
}

void UTPContext::onAccept(utp_socket *sock) {
	Nan::HandleScope scope;
	v8::Local<v8::Object> connObj = UTPSocket::create(this, sock)->handle();
	v8::Local<v8::Function> onConn = Nan::Get(handle(), Nan::New("_onConnect").ToLocalChecked()).ToLocalChecked().As<v8::Function>();
	v8::Local<v8::Value> argv[1] = {connObj};
	Nan::Callback(onConn).Call(1, argv);
}


NAN_METHOD(UTPContext::Init) {
	Nan::HandleScope scope;

	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
	tpl->SetClassName(Nan::New("UTPContext").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	constructor.Reset(tpl->GetFunction());
}

NAN_METHOD(UTPContext::New) {
	Nan::HandleScope scope;
	info.GetReturnValue().Set(info.This());
}

NAN_METHOD(UTPContext::Bind) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	unsigned int port = Nan::To<v8::Uint32>(info[0]).ToLocalChecked()->Value();
	string host(*Nan::Utf8String(info[1]));
	if (utpctx->state != STATE_INIT) {
		Nan::ThrowError("socket has already been bound");
		return;
	}
	int result = utpctx->bind(static_cast<uint16_t>(port), host);
	if (result == 0) {
		info.GetReturnValue().Set(Nan::New<v8::Boolean>(true));
	}
	v8::Local<v8::Value> err = Nan::Error(uv_strerror(result));
	err.As<v8::Object>()->Set(Nan::New("code").ToLocalChecked(), Nan::New(uv_err_name(result)).ToLocalChecked());
	Nan::ThrowError(err);
}

NAN_METHOD(UTPContext::Listen) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	if (utpctx->state != STATE_BOUND) {
		Nan::ThrowError("invalid socket state");
		return;
	}
	int backlog = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();
	utpctx->listen(backlog);
}

NAN_METHOD(UTPContext::Connect) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	unsigned int port = Nan::To<v8::Uint32>(info[0]).ToLocalChecked()->Value();
	string host(*Nan::Utf8String(info[1]));
	if (utpctx->state != STATE_BOUND) {
		Nan::ThrowError("invalid socket state");
		return;
	}
	UTPSocket *utpsock = utpctx->connect(static_cast<uint16_t>(port), host);
	if (!utpsock) {
		Nan::ThrowError("fail to connect");
		return;
	}
	info.GetReturnValue().Set(utpsock->handle());
}

NAN_METHOD(UTPContext::Close) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	utpctx->stop();
}
