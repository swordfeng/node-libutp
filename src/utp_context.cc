#include "utp.h"

UTPContext::UTPContext(): ctx(utp_init(2), utp_destroy) {
	assert(uv_udp_init(uv_default_loop(), &udpHandle) >= 0);
	assert(uv_timer_init(uv_default_loop(), &timerHandle) >= 0);

	utp_context_set_userdata(ctx.get(), this);
	udpHandle.data = this;
	timerHandle.data = this;

	for (int type: vector<int>({UTP_SENDTO, UTP_ON_ERROR, UTP_ON_STATE_CHANGE, UTP_ON_READ, UTP_ON_FIREWALL, UTP_ON_ACCEPT})) {
		utp_set_callback(ctx.get(), type, &onCallback);
	}
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
	state = STATE_BOUND;
	return 0;
};

UTPContext::~UTPContext() {
	if (state == STATE_BOUND || state == STATE_STOPPED) {
		assert(uv_timer_stop(&timerHandle) >= 0);
		assert(uv_udp_recv_stop(&udpHandle) >= 0);
	}
	uv_close(reinterpret_cast<uv_handle_t *>(&udpHandle), nullptr);
	uv_close(reinterpret_cast<uv_handle_t *>(&timerHandle), nullptr);
}

int UTPContext::listen(int _backlog) {
	if (state != STATE_BOUND) return -1;
	backlog = _backlog;
	if (backlog < 0) backlog = 0;
	listening = true;
	return 0;
}

uint64 UTPContext::onCallback(utp_callback_arguments *a) {
	UTPContext *utpctx = static_cast<UTPContext *>(utp_context_get_userdata(a->context));
	UTPSocket *utpsock;
	switch (a->callback_type) {
	case UTP_SENDTO:
		return utpctx->sendTo(a->buf, a->len, a->address, a->address_len);
	case UTP_ON_FIREWALL:
		return static_cast<uint64>(utpctx->onFirewall());
	case UTP_ON_ACCEPT:
		utpctx->connections++;
		utpctx->onAccept(a->socket);
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
			utpctx->connections--;
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
	v8::Local<v8::Function> onConn = Nan::Get(handle(), Nan::New<v8::String>("_onConnect").ToLocalChecked()).ToLocalChecked().As<v8::Function>();
	v8::Local<v8::Value> argv[1] = {connObj};
	Nan::Callback(onConn).Call(1, argv);
}


NAN_METHOD(UTPContext::Init) {
	Nan::HandleScope scope;

	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(isolate, New);
	tpl->SetClassName(Nan::New("UTPContext").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	constructor.Reset(tpl->GetFunction());
}

NAN_METHOD(UTPContext::New) {
	Nan::HandleScope scope;
	v8::Isolate *isolate = info.GetIsolate();
	info.GetReturnValue().Set(info.This());
}

NAN_METHOD(UTPContext::Bind) {
	Nan::HandleScope scope;
	v8::Isolate *isolate = info.GetIsolate();
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.This());
	Nan::MaybeLocal<v8::UInt32> port = info[0];
	string host = Nan::Utf8String(info[1])();
	if (utpctx->state != STATE_INIT) {
		info.GetReturnValue().Set(v8::Boolean::New(isolate, false));
		return;
	}
	int result = utpctx->bind(static_cast<uint16_t>(port->Value()), host);
	if (result == 0) {}
		info.GetReturnValue().Set(v8::Boolean::New(isolate, true));
	}
	v8::Local<v8::Error> err = Nan::Error(uv_strerror(result));
	err->Set(Nan::New("code"), Nan::New(uv_err_name(result)));
	Nan::ThrowError(err);
}
